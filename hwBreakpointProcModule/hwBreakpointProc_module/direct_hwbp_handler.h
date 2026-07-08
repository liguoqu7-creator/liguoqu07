#ifndef _DIRECT_HWBP_HANDLER_H_
#define _DIRECT_HWBP_HANDLER_H_

/*
 * direct_hwbp_handler.h — Hook do_debug_exception + 单步恢复
 *
 * 通过 kretprobe 拦截 do_debug_exception，完全绕过 perf_event 框架。
 * 异常触发 → 解析 ESR → 匹配我们的断点 → 记录命中 → SS 恢复。
 *
 * 遵循 anti_ptrace_detection.h 的 kretprobe 模式。
 *
 * 【包含依赖】此文件必须 include 在 .c 文件中 record_hit_details、g_hwbp_handle_info_mutex、
 *            g_hwbp_handle_info_arr 定义之后，才能访问这些 static 符号。
 *            同时 g_hook_pc 需为非 static 声明。
 */

#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/threads.h>  // NR_CPUS
#include "direct_hwbp_install.h"

// ======================== 外部符号 ========================
// 定义在 hwBreakpointProc_module.c / hwbp_proc.c 中
extern atomic64_t g_hook_pc;

// ======================== per-CPU 单步恢复状态 ========================

struct per_cpu_step_state {
    pid_t    stepping_pid;       // 正在单步的进程 PID，0 = 空闲
    int      stepping_slot;      // 触发槽位号
    bool     stepping_is_wp;     // true=watchpoint, false=breakpoint
    bool     migration_disabled; // 是否已调用 migrate_disable
};

static struct per_cpu_step_state g_per_cpu_step[NR_CPUS];

// ======================== 全局句柄数组引用 ========================
// 由 start_direct_hwbp_handler() 初始化

static struct mutex *g_p_direct_hwbp_mutex = NULL;
static cvector      *g_p_direct_hwbp_cv    = NULL;

// ======================== kretprobe 实例数据 ========================

struct hwbp_debug_data {
    unsigned long esr_save;
    bool intercepted;
};

// ======================== 句柄查找 ========================
// 通过对齐后的硬件地址匹配 HWBP_HANDLE_INFO（直接模式条目：sample_hbp==NULL）

static struct HWBP_HANDLE_INFO *
direct_hwbp_find_handle_by_addr(uint64_t hw_addr, bool is_watchpoint) {
    citerator iter;
    struct HWBP_HANDLE_INFO *found = NULL;

    if (!g_p_direct_hwbp_mutex || !g_p_direct_hwbp_cv)
        return NULL;

    mutex_lock(g_p_direct_hwbp_mutex);
    for (iter = cvector_begin(*g_p_direct_hwbp_cv);
         iter != cvector_end(*g_p_direct_hwbp_cv);
         iter = cvector_next(*g_p_direct_hwbp_cv, iter))
    {
        struct HWBP_HANDLE_INFO *info = (struct HWBP_HANDLE_INFO *)iter;
        // 只处理直接模式条目：sample_hbp 为伪句柄（小整数 < THRESHOLD）
        // 跳过 perf_event 条目（sample_hbp 是真正的内核指针 >= THRESHOLD）
        if (info->sample_hbp == NULL ||
            (uintptr_t)info->sample_hbp >= DIRECT_HWBP_HANDLE_THRESHOLD)
            continue;

        uint64_t check_addr = calc_hw_addr(&info->original_attr,
                                            info->is_32bit_task);
        if (check_addr == hw_addr) {
            found = info;
            break;
        }
    }
    mutex_unlock(g_p_direct_hwbp_mutex);
    return found;
}

// ======================== 当前 CPU 上所有直接模式断点的批量开关 ========================
// 仅在单步转换期间使用

static void direct_hwbp_toggle_all_on_cpu(bool enable) {
    citerator iter;
    if (!g_p_direct_hwbp_mutex || !g_p_direct_hwbp_cv)
        return;

    mutex_lock(g_p_direct_hwbp_mutex);
    for (iter = cvector_begin(*g_p_direct_hwbp_cv);
         iter != cvector_end(*g_p_direct_hwbp_cv);
         iter = cvector_next(*g_p_direct_hwbp_cv, iter))
    {
        struct HWBP_HANDLE_INFO *info = (struct HWBP_HANDLE_INFO *)iter;
        // 只处理直接模式条目（伪句柄 < THRESHOLD），跳过 perf_event 条目
        if (info->sample_hbp == NULL ||
            (uintptr_t)info->sample_hbp >= DIRECT_HWBP_HANDLE_THRESHOLD)
            continue;

        toggle_bp_registers_directly(&info->original_attr,
                                      info->is_32bit_task, enable ? 1 : 0);
    }
    mutex_unlock(g_p_direct_hwbp_mutex);
}

// ======================== kretprobe entry_handler ========================
// 在 do_debug_exception 执行前拦截。
// ARM64 调用约定：arg0=addr(regs[0]), arg1=esr(regs[1]), arg2=pt_regs(regs[2])
// kprobe 框架已禁用抢占并绑定 CPU，smp_processor_id() 安全

static int entry_do_debug_exception(struct kretprobe_instance *ri,
                                     struct pt_regs *regs)
{
    unsigned long addr   = regs->regs[0];   // faulting/data address
    unsigned long esr    = regs->regs[1];   // ESR_EL1
    struct pt_regs *target_regs = (struct pt_regs *)regs->regs[2]; // 目标进程 pt_regs
    unsigned int evt = esr_to_debug_evt(esr);
    struct hwbp_debug_data *data = (struct hwbp_debug_data *)ri->data;
    int cpu;
    struct per_cpu_step_state *cpu_state;
    uint64_t hook_pc;

    data->esr_save = esr;
    data->intercepted = false;

    // kprobe 框架已禁用抢占且绑定 CPU，smp_processor_id() 安全
    cpu = smp_processor_id();
    if (cpu >= NR_CPUS) {
        return 0;  // 不应该发生
    }
    cpu_state = &g_per_cpu_step[cpu];

    // --- 1. SetHookPC：无条件 PC 跳转（与 perf_event 路径共享逻辑）---
    hook_pc = atomic64_read(&g_hook_pc);
    if (hook_pc) {
        target_regs->pc = hook_pc;
        data->intercepted = true;
        regs->regs[1] = 0;  // 清零 ESR，原函数无害返回
        return 0;
    }

    // --- 2. 单步恢复 ---
    if (evt == ESR_EVT_HWSS) {
        if (cpu_state->stepping_pid != 0) {
            // 清除 MDSCR_EL1 单步
            uint64_t mdscr = read_mdscr_el1();
            mdscr &= ~DBG_MDSCR_SS;
            write_mdscr_el1(mdscr);

            // 清除 SPSR.SS
            clear_spsr_ss_bit(target_regs);

            // 重新启用当前 CPU 上我们所有的断点
            direct_hwbp_toggle_all_on_cpu(true);

            // 恢复进程迁移
            if (cpu_state->migration_disabled) {
                if (g_migrate_enable_sym) {
                    g_migrate_enable_sym();
                }
                cpu_state->migration_disabled = false;
            }

            cpu_state->stepping_pid = 0;
            cpu_state->stepping_slot = -1;

            data->intercepted = true;
            regs->regs[1] = 0;
            return 0;
        }
    }

    // --- 3. 监视点命中 ---
    if (evt == ESR_EVT_HWWP) {
        uint64_t aligned_addr = addr & ~0x7ULL;
        struct HWBP_HANDLE_INFO *info;

        info = direct_hwbp_find_handle_by_addr(aligned_addr, true);
        if (info) {
            // 记录命中（对监视点，addr 是 faulting address 而非 PC）
            info->hit_total_count++;
            record_hit_details(info, target_regs, addr);

            // 禁止进程迁移
            if (g_migrate_disable_sym && !cpu_state->migration_disabled) {
                g_migrate_disable_sym();
                cpu_state->migration_disabled = true;
            }

            // 禁用当前 CPU 上的所有我们自己的断点（防止死循环）
            direct_hwbp_toggle_all_on_cpu(false);

            // 设置 SPSR.SS + MDSCR_EL1.SS，返回用户态后执行一条指令再触发
            set_spsr_ss_bit(target_regs);
            uint64_t mdscr = read_mdscr_el1();
            mdscr |= DBG_MDSCR_SS;
            write_mdscr_el1(mdscr);

            cpu_state->stepping_pid = current->pid;
            cpu_state->stepping_slot = info->direct_slot.slot_index;
            cpu_state->stepping_is_wp = true;

            data->intercepted = true;
            regs->regs[1] = 0;  // 清零 ESR
            return 0;
        }
    }

    // --- 4. 执行断点命中 ---
    if (evt == ESR_EVT_HWBP) {
        // 对于执行断点，addr 就是 PC
        uint64_t bp_addr = target_regs->pc;
        struct HWBP_HANDLE_INFO *info;

        info = direct_hwbp_find_handle_by_addr(bp_addr, false);
        if (info) {
            // 记录命中（对执行断点，hit_addr = PC）
            info->hit_total_count++;
            record_hit_details(info, target_regs, target_regs->pc);

            // 禁止进程迁移
            if (g_migrate_disable_sym && !cpu_state->migration_disabled) {
                g_migrate_disable_sym();
                cpu_state->migration_disabled = true;
            }

            // 禁用当前 CPU 上这个执行断点
            toggle_bp_registers_directly(&info->original_attr, info->is_32bit_task, 0);

            // 设置单步以越过断点指令
            set_spsr_ss_bit(target_regs);
            uint64_t mdscr = read_mdscr_el1();
            mdscr |= DBG_MDSCR_SS;
            write_mdscr_el1(mdscr);

            cpu_state->stepping_pid = current->pid;
            cpu_state->stepping_slot = info->direct_slot.slot_index;
            cpu_state->stepping_is_wp = false;

            data->intercepted = true;
            regs->regs[1] = 0;
            return 0;
        }
    }

    // 不是我们的断点 → 原样执行 do_debug_exception
    return 0;
}

// ======================== kretprobe ret_handler ========================
// 我们的拦截事件已通过 ESR=0 处理完毕，原函数实际上什么都没做。
// ret_handler 无需额外操作。

static int ret_do_debug_exception(struct kretprobe_instance *ri,
                                   struct pt_regs *regs) {
    return 0;
}

// ======================== kretprobe 定义 ========================

static struct kretprobe kretp_do_debug_exception = {
    .kp.symbol_name = "do_debug_exception",
    .data_size      = sizeof(struct hwbp_debug_data),
    .entry_handler  = entry_do_debug_exception,
    .handler        = ret_do_debug_exception,
    .maxactive      = 20,
};

// ======================== 启动 / 停止 ========================

static bool start_direct_hwbp_handler(struct mutex *p_mutex, cvector *p_cv) {
    int ret;

    g_p_direct_hwbp_mutex = p_mutex;
    g_p_direct_hwbp_cv    = p_cv;

    memset(g_per_cpu_step, 0, sizeof(g_per_cpu_step));

    ret = register_kretprobe(&kretp_do_debug_exception);
    if (ret < 0) {
        printk_debug(KERN_INFO "register_kretprobe(do_debug_exception) failed: %d\n", ret);
        return false;
    }
    printk_debug(KERN_INFO "kretprobe on do_debug_exception registered, addr: %lx\n",
                 (unsigned long)kretp_do_debug_exception.kp.addr);
    return true;
}

static void stop_direct_hwbp_handler(void) {
    if (kretp_do_debug_exception.kp.addr) {
        unregister_kretprobe(&kretp_do_debug_exception);
        printk_debug(KERN_INFO "kretprobe on do_debug_exception unregistered\n");
    }
    g_p_direct_hwbp_mutex = NULL;
    g_p_direct_hwbp_cv    = NULL;
}

#endif /* _DIRECT_HWBP_HANDLER_H_ */
