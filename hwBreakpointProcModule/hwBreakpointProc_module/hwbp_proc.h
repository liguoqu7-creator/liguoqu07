#ifndef _HWBP_PROC_H_
#define _HWBP_PROC_H_
#include "ver_control.h"
#ifdef CONFIG_DIRECT_HWBP_MODE
#include "direct_hwbp_core.h"
#endif

// 直接模式句柄阈值：小于此值的句柄为直接模式伪句柄（非 perf_event 指针）
// perf_event 指针在内核地址空间，远大于此值
#define DIRECT_HWBP_HANDLE_THRESHOLD  0x10000ULL

#pragma pack(1)
struct my_user_pt_regs {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint64_t orig_x0;
	uint64_t syscallno;
};
struct HWBP_HIT_ITEM {
	uint64_t task_id;
	uint64_t hit_addr;
	uint64_t hit_time;
	struct my_user_pt_regs regs_info;
};
#pragma pack()

struct HWBP_HANDLE_INFO {
	uint64_t task_id;
	struct perf_event * sample_hbp;
	struct perf_event_attr original_attr;
	bool is_32bit_task;
#ifdef CONFIG_MODIFY_HIT_NEXT_MODE
	struct perf_event_attr next_instruction_attr;
#endif
	size_t hit_total_count;
	cvector hit_item_arr;
#ifdef CONFIG_DIRECT_HWBP_MODE
	struct direct_hwbp_slot direct_slot; // 直接模式槽位信息 (sample_hbp==NULL 时有效)
#endif
};

#endif