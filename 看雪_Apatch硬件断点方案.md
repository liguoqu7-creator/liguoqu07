# [原创]基于Apatch模块的安卓Linux内核硬件断点

> **来源**: 看雪安全社区 bbs.kanxue.com  
> **帖子**: https://bbs.kanxue.com/thread-289706.htm  
> **发布时间**: 2026-01-08  
> **作者设备**: 一加 Ace 5 Pro、Linux 6.16  
> **Root方案**: Apatch（直接对内核打补丁）  

---

## 1. 背景与整体思路

作者在处理某大厂App的检测时，发现常规的 `register_user_hw_breakpoint()` 等内核函数会通过 **perf_event 框架**在进程的 `thread_struct` 中留下调试痕迹。目标进程通过检查 `/proc` 下的调试状态即可发现被追踪。

因此实现了一套绕过 perf_event 框架的"无痕"方案：

```
设置CPU调试寄存器 → Hook do_debug_exception接管异常 → 过滤/处理我们的断点 → 单步恢复
```

---

## 2. BTF 解析结构体偏移

BTF（BPF Type Format）是 Linux 内核内置的类型信息格式（类似 Windows 的 PDB），内核版本 ≥ 5.2 即可使用。用于动态获取结构体字段偏移，无需为每个内核版本硬编码。

```c
// 初始化：获取 btf_vmlinux 指针
g_btf_vmlinux_sym = kallsyms_lookup_name("btf_vmlinux");
g_btf_vmlinux = *(struct btf **)g_btf_vmlinux_sym;

// 使用：获取 task_struct->pid 的偏移
int offset = kr_btf_offsetof("task_struct", "pid");

// 读取当前进程 PID
void *task = get_current_task();
int pid = *(int*)((char*)task + offset);
```

**优势**：内核版本升级后结构体偏移变化，BTF 自动适配，无需手动逆向。

---

## 3. 设置调试寄存器（ARM64 per-CPU）

ARM64 调试寄存器分为两组：
- **监视点（Watchpoint）**：DBGWVR（地址）+ DBGWCR（控制）
- **断点（Breakpoint）**：DBGBVR（地址）+ DBGBCR（控制）

**每个 CPU 核心独立一套**，这是多核同步的根本原因。

### 写入寄存器的内联汇编

```c
// 写入监视点值寄存器 DBGWVR<n>_EL1
static inline void write_dbgwvr(int n, unsigned long val) {
    switch(n) {
        case 0: asm volatile("msr dbgwvr0_el1, %0" :: "r"(val)); break;
        case 1: asm volatile("msr dbgwvr1_el1, %0" :: "r"(val)); break;
        case 2: asm volatile("msr dbgwvr2_el1, %0" :: "r"(val)); break;
        case 3: asm volatile("msr dbgwvr3_el1, %0" :: "r"(val)); break;
        // ...
    }
    asm volatile("isb");  // 指令同步屏障，必须加！
}

// 写入监视点控制寄存器 DBGWCR<n>_EL1
static inline void write_dbgwcr(int n, unsigned long val) {
    switch(n) {
        case 0: asm volatile("msr dbgwcr0_el1, %0" :: "r"(val)); break;
        case 1: asm volatile("msr dbgwcr1_el1, %0" :: "r"(val)); break;
        case 2: asm volatile("msr dbgwcr2_el1, %0" :: "r"(val)); break;
        case 3: asm volatile("msr dbgwcr3_el1, %0" :: "r"(val)); break;
        // ...
    }
    asm volatile("isb");
}
```

### DBGWCR 控制位说明

| 位 | 字段 | 含义 |
|---|------|------|
| bit 0 | E | 启用位 |
| bit 1-2 | PAC | 权限：0=EL0, 1=EL1, 2=EL2, 3=EL0+EL1 |
| bit 3-4 | LSC | 1=读, 2=写, 3=读写 |
| bit 5-12 | BAS | 字节选择掩码，0xFF=8字节 |

### 关键：必须启用 MDSCR_EL1 的 MDE 位（bit 15）

```c
// 在单个 CPU 上设置监视点
static void set_watchpoint_on_cpu(void *info) {
    int slot = ((int*)info)[0];
    unsigned long addr = g_watchpoints[slot].address;
    int type = g_watchpoints[slot].type;
    int len = g_watchpoints[slot].len;
    unsigned long wcr;
    unsigned long mdscr;
    unsigned long aligned_addr;

    // 8 字节对齐
    aligned_addr = (addr & 0x00FFFFFFFFFFFFFFULL) & ~0x7UL;

    // 启用 MDSCR_EL1 的 MDE 位（bit 15）
    mdscr = read_mdscr_el1();
    if (!(mdscr & DBG_MDSCR_MDE)) {
        mdscr |= DBG_MDSCR_MDE;
        write_mdscr_el1(mdscr);
    }

    write_dbgwvr(slot, aligned_addr);
    wcr = build_wcr(type, len, addr);
    write_dbgwcr(slot, wcr);
}
```

**如果不设 MDE 位，断点不会触发任何异常。**

---

## 4. 多核同步：smp_call_function

因为调试寄存器是 per-CPU 的，进程可能被调度到任何核心。必须在所有 CPU 核心上同步设置断点：

```c
// 先在当前 CPU 执行
slot_arg = slot;
set_watchpoint_on_cpu(&slot_arg);

// 再在其他所有 CPU 上执行
if (fn_smp_call_function)
    fn_smp_call_function(set_watchpoint_on_cpu, &slot_arg, 1);
```

---

## 5. 异常接管：Hook do_debug_exception

使用 Apatch 框架的 Hook 机制，劫持 `do_debug_exception` 函数。

### ESR 寄存器解析

ESR（Exception Syndrome Register）的 bit [29:27] 区分异常类型：

| bit[29:27] | 含义 |
|------------|------|
| 0x0 | 硬件断点 (Breakpoint) |
| 0x1 | 硬件单步 (Single Step) |
| 0x2 | 硬件监视点 (Watchpoint) |

### Hook 实现

```c
// do_debug_exception 原型:
// void do_debug_exception(unsigned long addr, unsigned long esr, struct pt_regs *regs)

static void do_debug_exception_before(hook_fargs3_t *args, void *udata) {
    unsigned long addr = args->arg0;
    unsigned long esr = args->arg1;
    void *regs = (void*)args->arg2;

    // 从 ESR 提取事件类型
    unsigned long evt = (esr >> 27) & 0x7;

    // 监视点异常
    if (evt == 0x2) {
        int slot = find_our_watchpoint(addr);
        if (slot >= 0) {
            // 是我们的监视点，记录命中信息
            record_watchpoint_hit(slot, addr, esr, regs);
            // 设置单步恢复（见下一节）
            args->skip_origin = 1;  // 不调用原函数，阻止SIGTRAP发给进程
            return;
        }
    }
    // 不是我们的断点，交给原函数处理
}

// 安装 Hook
g_do_debug_exception_addr = kallsyms_lookup_name("do_debug_exception");
hook_wrap3(g_do_debug_exception_addr, do_debug_exception_before, NULL, NULL);
```

---

## 6. 异常处理与单步恢复（最复杂的坑）

监视点触发后，**如果直接返回，PC 还指向触发指令，会再次触发 → 死循环 → 进程收到 SIGTRAP 被杀**。

解决方案：**临时禁用监视点 → 设单步标志 → 执行一条指令 → 单步异常触发 → 恢复监视点**。

同时还要处理 **进程跨 CPU 迁移** 的问题——如果在单步期间进程被调度到另一个核心，另一个核心上没有单步状态，会导致逻辑混乱或 kernel panic。

```c
static void do_debug_exception_before(hook_fargs3_t *args, void *udata) {
    unsigned long addr = args->arg0;
    unsigned long esr = args->arg1;
    void *regs = (void*)args->arg2;
    unsigned long evt = (esr >> 27) & 0x7;
    per_cpu_step_state_t *cpu_state = get_cpu_step_state();

    // ========== 【单步异常：恢复阶段】==========
    if (evt == ESR_EVT_HWSS) {
        if (cpu_state->stepping_pid != 0) {
            // 清除 MDSCR_EL1 单步标志
            unsigned long mdscr = read_mdscr_el1();
            mdscr &= ~DBG_MDSCR_SS;
            write_mdscr_el1(mdscr);

            // 重新启用我们在这个CPU上的所有监视点
            enable_our_watchpoints_on_cpu();

            // 恢复进程迁移能力
            if (cpu_state->migration_disabled && fn_migrate_enable) {
                fn_migrate_enable();
                cpu_state->migration_disabled = 0;
            }

            cpu_state->stepping_pid = 0;
            args->skip_origin = 1;
            return;
        }
    }

    // ========== 【监视点异常：触发阶段】==========
    if (evt == ESR_EVT_HWWP) {
        int slot = find_our_watchpoint(addr);
        if (slot >= 0) {
            // 记录命中
            record_watchpoint_hit(slot, addr, esr, regs);

            // ★关键★：禁止进程迁移到其他CPU
            if (fn_migrate_disable && !cpu_state->migration_disabled) {
                fn_migrate_disable();
                cpu_state->migration_disabled = 1;
            }

            // 禁用当前CPU上的监视点（防止死循环）
            disable_our_watchpoints_on_cpu();

            // 设置返回用户态后的单步标志（SPSR.SS）
            unsigned long *pstate_ptr = (unsigned long*)((char*)regs + 0x108);
            *pstate_ptr |= DBG_SPSR_SS;

            // 设置 MDSCR_EL1 单步使能
            unsigned long mdscr = read_mdscr_el1();
            mdscr |= DBG_MDSCR_SS;
            write_mdscr_el1(mdscr);

            cpu_state->stepping_pid = get_current_pid();
            cpu_state->stepping_slot = slot;
            args->skip_origin = 1;
            return;
        }
    }
}
```

### 踩坑总结

| 问题 | 原因 | 解决 |
|------|------|------|
| 断点不触发 | MDSCR_EL1 的 MDE 位未使能 | 写寄存器前先 set MDE bit |
| 进程被杀(SIGTRAP) | 断点反复触发，异常未处理 | Hook do_debug_exception + skip_origin |
| 死循环 | 未做单步恢复，PC 反复指向触发指令 | SPSR.SS + MDSCR.SS 单步跳过后再恢复断点 |
| 单步时 kernel panic | 进程在单步期间被调度到其他CPU | migrate_disable() 禁止迁移 |
| 断点在其他核不生效 | 调试寄存器是 per-CPU 的 | smp_call_function 全核同步 |

---

## 7. 断下后的操作：调用栈回溯 & 寄存器读取

### FP 链遍历用户态调用栈

ARM64 调用约定：X29 = FP（帧指针），每帧布局 `[prev_fp, return_addr]`。

```c
void *regs = (void*)args->arg2;

// 从 pt_regs 获取关键寄存器
unsigned long pc = *(unsigned long*)((char*)regs + 0x100);  // PC
unsigned long lr = *(unsigned long*)((char*)regs + 0xF0);   // X30/LR
unsigned long fp = *(unsigned long*)((char*)regs + 0xE8);   // X29/FP

// 遍历 FP 链
int depth = 0;
while (fp != 0 && depth < MAX_DEPTH) {
    unsigned long frame[2];
    if (kr_copy_from_user(frame, (void __user *)fp, sizeof(frame)) != 0)
        break;
    
    pr_info("[%d] LR = 0x%lx\n", depth, frame[1]);  // 返回地址
    fp = frame[0];  // 上一帧的FP
    depth++;
}
```

### 函数参数读取（ARM64 前8个参数走 X0-X7）

```c
// pt_regs 布局: X0-X30 依次排列，每个 8 字节
// regs 偏移量对应关系：
unsigned long x0 = *(unsigned long*)((char*)regs + 0x00);  // 第1个参数
unsigned long x1 = *(unsigned long*)((char*)regs + 0x08);  // 第2个参数
unsigned long x2 = *(unsigned long*)((char*)regs + 0x10);  // 第3个参数
unsigned long x3 = *(unsigned long*)((char*)regs + 0x18);  // 第4个参数
unsigned long x4 = *(unsigned long*)((char*)regs + 0x20);  // 第5个参数
unsigned long x5 = *(unsigned long*)((char*)regs + 0x28);  // 第6个参数
unsigned long x6 = *(unsigned long*)((char*)regs + 0x30);  // 第7个参数
unsigned long x7 = *(unsigned long*)((char*)regs + 0x38);  // 第8个参数
```

---

## 8. 总结

| 要点 | 说明 |
|------|------|
| **无痕性** | 直接操作 CPU 调试寄存器，不经过 perf_event，目标进程无法在 thread_struct 中检测到调试痕迹 |
| **BTF** | 动态获取内核结构体偏移，内核 ≥ 5.2 即可，无需为每个版本硬编码 |
| **多核同步** | 用 `smp_call_function` 在所有 CPU 核心上设置断点 |
| **单步恢复** | 触发后设 SPSR.SS + MDSCR.SS 单步 + `migrate_disable` 防止跨 CPU 迁移 |
| **isb 屏障** | 每次写调试寄存器后必须执行，否则修改不生效 |
| **ESR 解析** | bit[29:27] 区分断点(0x0)/单步(0x1)/监视点(0x2) |
| **MDE 位** | MDSCR_EL1 bit15 必须置位，否则断点不会触发异常 |
| **函数参数** | ARM64 前8个参数在 X0-X7，可直接从 pt_regs 读取 |
| **调用栈** | 通过 X29/FP 链遍历用户态栈帧回溯 |

---

> **原文链接**: https://bbs.kanxue.com/thread-289706.htm  
> **注意**: 此文件是根据搜索引擎缓存和摘要整理的技术要点，非逐字复制。完整原始内容建议在能访问看雪时直接查看原帖。
