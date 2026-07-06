#ifndef _DIRECT_HWBP_INSTALL_H_
#define _DIRECT_HWBP_INSTALL_H_

/*
 * direct_hwbp_install.h — 直接模式槽位管理 + 多核安装
 *
 * 提供：
 *   - 空闲硬件槽位查找
 *   - 在单个 CPU 上写入调试寄存器（smp_call_function 回调）
 *   - 全核安装 / 禁用 / 启用
 */

#include "direct_hwbp_core.h"

// ======================== smp_call_function 回调数据 ========================

struct smp_bp_data {
    int      slot_index;
    bool     is_watchpoint;
    uint64_t hw_addr;
    uint32_t hw_ctrl;
    bool     enable;           // true=install/enable, false=disable
};

// ======================== 单 CPU 写入回调 ========================
// 由 direct_hwbp_install / enable / disable 调用（本地 + smp_call_function 远程）

static void direct_hwbp_write_cpu(void *info) {
    struct smp_bp_data *d = (struct smp_bp_data *)info;
    int val_reg, ctrl_reg;

    ensure_mdscr_mde();

    if (d->is_watchpoint) {
        val_reg  = AARCH64_DBG_REG_WVR;
        ctrl_reg = AARCH64_DBG_REG_WCR;
    } else {
        val_reg  = AARCH64_DBG_REG_BVR;
        ctrl_reg = AARCH64_DBG_REG_BCR;
    }

    if (d->enable) {
        write_wb_reg(val_reg,  d->slot_index, d->hw_addr);
        write_wb_reg(ctrl_reg, d->slot_index, d->hw_ctrl);
    } else {
        // 仅清除 enable 位，保留其余控制位
        uint64_t ctrl = read_wb_reg(ctrl_reg, d->slot_index);
        ctrl &= ~1ULL;
        write_wb_reg(ctrl_reg, d->slot_index, ctrl);
    }
}

// ======================== 空闲槽位查找 ========================
// 扫描指定类型的所有槽位，返回第一个 enable bit=0 的槽位号
// 返回 -1 表示无空闲

static int direct_hwbp_find_free_slot(bool is_watchpoint) {
    int i, max_slots, ctrl_reg;

    if (is_watchpoint) {
        max_slots = getCpuNumWrps();
        ctrl_reg  = AARCH64_DBG_REG_WCR;
    } else {
        max_slots = getCpuNumBrps();
        ctrl_reg  = AARCH64_DBG_REG_BCR;
    }

    for (i = 0; i < max_slots; ++i) {
        uint64_t ctrl = read_wb_reg(ctrl_reg, i);
        if (!(ctrl & 1ULL)) {   // enable bit clear = 空闲
            return i;
        }
    }
    return -1;
}

// ======================== 全核安装 ========================
// 找到空闲槽位 → 构建控制字 → 写本地 CPU → smp_call 传播到其他 CPU
// 成功返回 true，失败（无空闲槽位）返回 false

static bool direct_hwbp_install(const struct perf_event_attr *attr,
                                 bool is_32bit_task,
                                 struct direct_hwbp_slot *out_slot) {
    int slot;
    bool is_wp;
    uint64_t hw_addr;
    uint32_t hw_ctrl;
    struct smp_bp_data data;

    is_wp = (attr->bp_type != HW_BREAKPOINT_X);
    hw_addr = calc_hw_addr(attr, is_32bit_task);

    slot = direct_hwbp_find_free_slot(is_wp);
    if (slot < 0) {
        printk_debug(KERN_INFO "direct_hwbp: no free slot (is_wp=%d)\n", is_wp);
        return false;
    }

    if (is_wp) {
        hw_ctrl = build_wcr(attr->bp_type, attr->bp_len, attr->bp_addr);
    } else {
        hw_ctrl = build_bcr(attr->bp_len, is_32bit_task);
    }

    data.slot_index    = slot;
    data.is_watchpoint = is_wp;
    data.hw_addr       = hw_addr;
    data.hw_ctrl       = hw_ctrl;
    data.enable        = true;

    // 本地 CPU 写入
    direct_hwbp_write_cpu(&data);

    // 传播到所有其他 CPU（smp_call_function 是可选符号）
    if (g_smp_call_function_sym) {
        g_smp_call_function_sym(direct_hwbp_write_cpu, &data, 1);
    }

    out_slot->slot_index    = slot;
    out_slot->is_watchpoint = is_wp;
    out_slot->hw_addr       = hw_addr;
    out_slot->hw_ctrl       = hw_ctrl;

    printk_debug(KERN_INFO "direct_hwbp: installed slot=%d addr=0x%llx wp=%d\n",
                 slot, hw_addr, is_wp);
    return true;
}

// ======================== 全核禁用 ========================

static void direct_hwbp_disable_slot(struct direct_hwbp_slot *slot) {
    struct smp_bp_data data;
    data.slot_index    = slot->slot_index;
    data.is_watchpoint = slot->is_watchpoint;
    data.hw_addr       = slot->hw_addr;
    data.hw_ctrl       = slot->hw_ctrl;
    data.enable        = false;

    direct_hwbp_write_cpu(&data);
    if (g_smp_call_function_sym) {
        g_smp_call_function_sym(direct_hwbp_write_cpu, &data, 1);
    }
}

// ======================== 全核启用 ========================

static void direct_hwbp_enable_slot(struct direct_hwbp_slot *slot) {
    struct smp_bp_data data;
    data.slot_index    = slot->slot_index;
    data.is_watchpoint = slot->is_watchpoint;
    data.hw_addr       = slot->hw_addr;
    data.hw_ctrl       = slot->hw_ctrl;
    data.enable        = true;

    direct_hwbp_write_cpu(&data);
    if (g_smp_call_function_sym) {
        g_smp_call_function_sym(direct_hwbp_write_cpu, &data, 1);
    }
}

#endif /* _DIRECT_HWBP_INSTALL_H_ */
