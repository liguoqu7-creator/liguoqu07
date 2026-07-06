#ifndef _DIRECT_HWBP_CORE_H_
#define _DIRECT_HWBP_CORE_H_

/*
 * direct_hwbp_core.h — ARM64 调试寄存器控制字构建 + MDSCR_EL1 操作
 *
 * 基于 arm64_register_helper.h 扩展，提供直接硬件断点模式所需的：
 *   - DBGWCR/DBGBCR 控制字构建
 *   - MDSCR_EL1 读写与 MDE/SS 位操作
 *   - SPSR 单步标志位操作
 *   - ESR 调试事件类型解析
 */

#include "arm64_register_helper.h"
#include <linux/hw_breakpoint.h>

// ======================== 直接模式槽位描述符 ========================
// 在 HWBP_HANDLE_INFO 中嵌入，描述一个已安装的硬件断点寄存器槽位

struct direct_hwbp_slot {
    uint8_t  slot_index;       // 硬件槽位号 (0..N-1)
    bool     is_watchpoint;    // true=WVR/WCR, false=BVR/BCR
    uint64_t hw_addr;          // 对齐后写入寄存器的地址
    uint32_t hw_ctrl;          // 控制字 (WCR 或 BCR)
};

// ======================== MDSCR_EL1 位定义 ========================
#define DBG_MDSCR_SS       (1UL << 0)    // Single-Step enable
#define DBG_MDSCR_MDE      (1UL << 15)   // Monitor Debug Enable (必须使能)

// ======================== SPSR (pstate) 单步位 ========================
#define DBG_SPSR_SS        (1UL << 21)   // ARM64 SPSR.SS bit

// ======================== ESR_EL1 调试事件类型 (bit[29:27]) ========================
#define ESR_EVT_HWBP       0x0           // Hardware Breakpoint (执行断点)
#define ESR_EVT_HWSS       0x1           // Hardware Single-Step
#define ESR_EVT_HWWP       0x2           // Hardware Watchpoint (访问监视点)

// ======================== DBGWCR 控制位布局 ========================
// bit 0      : E    (Enable)
// bit 1-2    : PAC  (Privilege Access Control)
// bit 3-4    : LSC  (Load/Store Control)
// bit 5-12   : BAS  (Byte Address Select)

#define WCR_PAC_EL0         (0UL << 1)
#define WCR_PAC_EL1         (1UL << 1)
#define WCR_PAC_EL0EL1      (3UL << 1)   // EL0 or EL1

#define WCR_LSC_LOAD        (1UL << 3)   // Load only
#define WCR_LSC_STORE       (2UL << 3)   // Store only
#define WCR_LSC_LOADSTORE   (3UL << 3)   // Load + Store

// ======================== MDSCR_EL1 读写 ========================

static inline uint64_t read_mdscr_el1(void) {
    uint64_t val;
    asm volatile("mrs %0, mdscr_el1" : "=r"(val));
    return val;
}

static inline void write_mdscr_el1(uint64_t val) {
    asm volatile("msr mdscr_el1, %0" :: "r"(val));
    isb();
}

// ======================== MDE 位确保使能 ========================
// 不设 MDE 位则调试异常不会触发，这是最基本的使能条件

static inline void ensure_mdscr_mde(void) {
    uint64_t mdscr = read_mdscr_el1();
    if (!(mdscr & DBG_MDSCR_MDE)) {
        mdscr |= DBG_MDSCR_MDE;
        write_mdscr_el1(mdscr);
    }
}

// ======================== SPSR 单步标志操作 ========================
// 在返回用户态前设置 SPSR.SS，CPU 会在执行一条指令后再次触发调试异常

static inline void set_spsr_ss_bit(struct pt_regs *regs) {
    regs->pstate |= DBG_SPSR_SS;
}

static inline void clear_spsr_ss_bit(struct pt_regs *regs) {
    regs->pstate &= ~DBG_SPSR_SS;
}

// ======================== ESR 事件类型解码 ========================

static inline unsigned int esr_to_debug_evt(unsigned long esr) {
    return (esr >> 27) & 0x7;
}

// ======================== DBGWCR 控制字构建 ========================
// bp_type: HW_BREAKPOINT_R / HW_BREAKPOINT_W / HW_BREAKPOINT_RW
// bp_len:  HW_BREAKPOINT_LEN_1 / 2 / 4 / 8
// addr:    原始(未对齐)虚拟地址, 用于计算 BAS 字节选择掩码

static inline uint32_t build_wcr(int bp_type, int bp_len, uint64_t addr) {
    uint32_t wcr = 0;
    uint32_t bas = 0;
    int byte_offset;

    // PAC: EL0 only (用户态调试)
    wcr |= WCR_PAC_EL0;

    // LSC: Load/Store type
    switch (bp_type) {
    case HW_BREAKPOINT_R:  wcr |= WCR_LSC_LOAD;       break;
    case HW_BREAKPOINT_W:  wcr |= WCR_LSC_STORE;      break;
    case HW_BREAKPOINT_RW: wcr |= WCR_LSC_LOADSTORE;  break;
    default: break;
    }

    // BAS: byte address select within 8-byte aligned window
    byte_offset = addr & 0x7;
    switch (bp_len) {
    case HW_BREAKPOINT_LEN_1: bas = (1U << byte_offset);           break;
    case HW_BREAKPOINT_LEN_2: bas = (3U << byte_offset);           break;
    case HW_BREAKPOINT_LEN_4: bas = (0xFU << byte_offset);         break;
    case HW_BREAKPOINT_LEN_8: bas = 0xFF;                          break;
    default: break;
    }
    wcr |= (bas << 5);

    // Enable
    wcr |= 1U;  // bit 0: E

    return wcr;
}

// ======================== DBGBCR 控制字构建 ========================
// 执行断点控制字比监视点简单：只关心 PAC 和 E 位

static inline uint32_t build_bcr(int bp_len, bool is_32bit) {
    uint32_t bcr = 0;

    // PAC: EL0 (32位任务兼容 EL0 in AArch32 or AArch64)
    if (is_32bit)
        bcr |= WCR_PAC_EL0EL1;
    else
        bcr |= WCR_PAC_EL0;

    // Enable
    bcr |= 1U;  // bit 0: E

    (void)bp_len;  // 执行断点不需要 len (匹配 PC 即可)
    return bcr;
}

#endif /* _DIRECT_HWBP_CORE_H_ */
