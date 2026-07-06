# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Two Linux ARM64 kernel modules (`.ko` drivers) for Android process memory manipulation and hardware breakpoint debugging. Both communicate with userspace via hidden `/proc` file nodes using MD5-hashed directory names.

## Building

Each module compiles independently with the kernel build system:

```bash
# Build either module
cd <module_dir>
make -C /path/to/kernel/source M=$PWD modules

# The Makefile is minimal:
obj-m += <module_name>.o
```

Before building, edit `ver_control.h` to set the target kernel version:
```c
#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,6,30)  // pick your kernel
```

## Architecture

### Module 1: rwProcMem33 — Process Memory Read/Write

Source: `rwProcMem33Module/rwProcMem_module/`

Communicates via `read()` on `/proc/<md5_key>/<md5_key>`. Each request is a `struct ioctl_request` header followed by variable-length data.

**Command flow** (in `rwProcMem_module.c`):
1. `CMD_INIT_DEVICE_INFO` — must call first. Passes auxv + process name; driver auto-detects kernel struct offsets (mmap_lock, map_count, cmdline, root, task list, pid).
2. `CMD_OPEN_PROCESS` — `get_proc_pid_struct(pid)` returns `struct pid*` as handle.
3. `CMD_READ_PROCESS_MEMORY` / `CMD_WRITE_PROCESS_MEMORY` — walks ARM64 page tables (PGD→P4D→PUD→PMD→PTE) to get physical address, then reads/writes via `xlate_dev_mem_ptr()`. Force mode (`param3=1`) temporarily modifies PTE read/write bits.
4. All other commands operate through the handle.

**Key sub-modules** (all `static inline` in headers, included into the .c entry point):
- `phy_mem.h` — page table walking, PTE manipulation, physical memory read/write
- `proc_maps.h` — VMA enumeration with per-kernel-version `#if` blocks (~20 versions from 3.10 to 6.6)
- `proc_list.h` — PID enumeration by walking `init_task` linked list
- `proc_root.h` / `proc_cmdline.h` / `proc_rss.h` — credential manipulation, command-line address, RSS
- `api_proxy.h` — thin wrappers over `__kmalloc`, `__arch_copy_from_user`, etc., avoiding standard exported symbols
- `hide_procfs_dir.h` — hides the procfs communication directory

**Anti-detection**:
- `list_del_init(&__this_module.list)` + `kobject_del` hides from lsmod/sysfs
- CFI bypass: hooks `__cfi_check_fn` and `__cfi_check_fail`

### Module 2: hwBreakpointProc — Hardware Breakpoint Debugging

Source: `hwBreakpointProcModule/hwBreakpointProc_module/`

Uses Linux's `perf_event` hw_breakpoint framework (`register_user_hw_breakpoint`) rather than raw register writes.

**Configuration flags** (in `ver_control.h`):
- `CONFIG_KALLSYMS_LOOKUP_NAME` — dynamic symbol resolution for non-exported kernel functions
- `CONFIG_MODIFY_HIT_NEXT_MODE` — on breakpoint hit, moves breakpoint to next instruction then restores (two-step single-step emulation)
- `CONFIG_ANTI_PTRACE_DETECTION_MODE` — kretprobe on `arch_ptrace` to filter own breakpoint addresses from `PTRACE_GETREGSET NT_ARM_HW_BREAK/WATCH` queries

**Key sub-modules**:
- `arm64_register_helper.h` — direct read/write of ARM64 debug registers (DBGBVR/DBGBCR/DBGWVR/DBGWCR) via `toggle_bp_registers_directly()`, plus CPU capability query (`getCpuNumBrps/Wrps` via `ID_AA64DFR0_EL1`)
- `anti_ptrace_detection.h` — kretprobe on `arch_ptrace` that intercepts debug register queries and filters out addresses belonging to our breakpoints
- `cvector.h` — simple C vector implementation for handle management
- `kallsyms_lookup_api.h` — runtime kallsyms resolution

**Breakpoint lifecycle**:
1. `CMD_INST_PROCESS_HWBP` → `register_user_hw_breakpoint()` with `hwbp_handler` callback → stores in `g_hwbp_handle_info_arr`
2. On hit: `hwbp_handler` records hit info (timestamp, full `pt_regs`) → toggles breakpoint register enable bit directly (or uses CONFIG_MODIFY_HIT_NEXT_MODE)
3. `CMD_SUSPEND`/`CMD_RESUME` via `modify_user_hw_breakpoint()`
4. `CMD_UNINST` → `unregister_hw_breakpoint()`

## Cross-Kernel-Version Strategy

Both modules use `#if MY_LINUX_VERSION_CODE == KERNEL_VERSION(x,y,z)` for APIs that change between kernel versions. The `proc_maps.h` file in rwProcMem33 is the most extreme example — ~20 complete copies of `get_proc_maps_list()` for different kernel versions, each handling differences in `vm_area_struct` layout, stack detection, and VDSO identification.

Additionally, rwProcMem33 uses auto-offset detection (`*_auto_offset.h` files) that compute struct field offsets at runtime from user-provided process metadata, avoiding hardcoded offsets for many structures.

## Communication Protocol

Both modules use the same unconventional scheme:
- `CONFIG_USE_PROC_FILE_NODE` creates `/proc/<md5_hash>/<md5_hash>`
- `CONFIG_PROC_NODE_AUTH_KEY` is the md5 hash (different key per module)
- Userspace calls `read(fd, buf, size)` — not `ioctl()` or `write()`
- data layout: `[ioctl_request header (29 bytes)] + [variable payload of buf_size bytes]`
- `ioctl_request.cmd` dispatches to handler; `param1/2/3` carry the handle, address, and flags
