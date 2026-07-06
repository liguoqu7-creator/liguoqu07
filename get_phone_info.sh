#!/system/bin/sh
################################################################################
# get_phone_info.sh — 一键获取手机内核/CPU/配置信息
#
# 用法:
#   adb push get_phone_info.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/get_phone_info.sh"
#
# 输出文件: /data/local/tmp/<内核版本号>.txt
#   adb pull /data/local/tmp/<内核版本号>.txt ./
################################################################################

# ── 准备工作 ──────────────────────────────────────────────────────
OUT="/data/local/tmp/$(uname -r | tr ' ' '_').txt"

echo "============================================================"  | tee $OUT
echo "  手机内核与硬件信息一键采集"                                   | tee -a $OUT
echo "  时间: $(date '+%Y-%m-%d %H:%M:%S')"                         | tee -a $OUT
echo "  输出文件: $OUT"                                              | tee -a $OUT
echo "============================================================"  | tee -a $OUT
echo ""                                                              | tee -a $OUT

# ── 第 1 组: 内核版本（最关键）──────────────────────────────────
section() { echo "" | tee -a $OUT; echo "========== $1 ==========" | tee -a $OUT; }

section "1. 内核版本 (编译时改 MY_LINUX_VERSION_CODE 用)"
echo "  uname -r : $(uname -r)"                                     | tee -a $OUT
echo "  uname -m : $(uname -m)"                                     | tee -a $OUT
cat /proc/version                                                   | tee -a $OUT

# ── 第 2 组: Android 系统信息 ────────────────────────────────────
section "2. Android 系统信息"
echo "  SDK Level  : $(getprop ro.build.version.sdk)"               | tee -a $OUT
echo "  Release    : $(getprop ro.build.version.release)"           | tee -a $OUT
echo "  Brand      : $(getprop ro.product.brand)"                   | tee -a $OUT
echo "  Model      : $(getprop ro.product.model)"                   | tee -a $OUT
echo "  Board      : $(getprop ro.product.board)"                   | tee -a $OUT
echo "  Chipset    : $(getprop ro.board.platform)"                  | tee -a $OUT
echo "  Build ID   : $(getprop ro.build.id)"                        | tee -a $OUT
echo "  Fingerprint: $(getprop ro.build.fingerprint)"               | tee -a $OUT
echo "  SELinux    : $(getenforce)"                                 | tee -a $OUT

# ── 第 3 组: CPU 信息 ───────────────────────────────────────────
section "3. CPU 信息"
echo "  Cores      : $(grep -c processor /proc/cpuinfo)"            | tee -a $OUT
echo "  Implementer: $(grep 'CPU implementer' /proc/cpuinfo | head -1)" | tee -a $OUT
echo "  Part       : $(grep 'CPU part' /proc/cpuinfo | head -1)"     | tee -a $OUT
echo "  Variant    : $(grep 'CPU variant' /proc/cpuinfo | head -1)"  | tee -a $OUT
echo "  Features   : $(grep Features /proc/cpuinfo | head -1)"      | tee -a $OUT
echo ""                                                              | tee -a $OUT
echo "  调试寄存器数量 (从 ID_AA64DFR0_EL1):"                        | tee -a $OUT
echo "  注意: 需要 ARM64 环境, 手机 shell 可能无法直接读 MSR"        | tee -a $OUT

# ── 第 4 组: 内核配置 ──────────────────────────────────────────
section "4. 内核编译配置 (关键项)"
if [ -f /proc/config.gz ]; then
    echo "  /proc/config.gz 存在, 正在解析..."                      | tee -a $OUT
    echo ""                                                         | tee -a $OUT
    echo "--- 模块支持 ---"                                         | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_MODULES='                | tee -a $OUT
    echo ""                                                         | tee -a $OUT
    echo "--- kprobes/kretprobes (Hook do_debug_exception 需要) ---" | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_KPROBES='                | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_KRETPROBES='             | tee -a $OUT
    echo ""                                                         | tee -a $OUT
    echo "--- kallsyms (动态符号解析需要) ---"                      | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_KALLSYMS='               | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_KALLSYMS_ALL='           | tee -a $OUT
    echo ""                                                         | tee -a $OUT
    echo "--- perf / hw_breakpoint (perf_event 模式用) ---"         | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_PERF_EVENTS='            | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_HW_BREAKPOINT='          | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_HAVE_HW_BREAKPOINT='     | tee -a $OUT
    echo ""                                                         | tee -a $OUT
    echo "--- migrate_disable (5.10+) ---"                          | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_PREEMPT_RT='             | tee -a $OUT
    echo ""                                                         | tee -a $OUT
    echo "--- 其他: 栈保护 / CFI ---"                               | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_STACKPROTECTOR'          | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_CFI_CLANG='              | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_COMPAT='                 | tee -a $OUT
    zcat /proc/config.gz | grep -E 'CONFIG_PROC_FS='                | tee -a $OUT
else
    echo "  /proc/config.gz 不存在!"                                | tee -a $OUT
    echo "  尝试其他位置:"                                           | tee -a $OUT
    ls /boot/config* 2>/dev/null                                    | tee -a $OUT
    find /vendor -name "config*" 2>/dev/null | head -5              | tee -a $OUT
fi

# ── 第 5 组: 内核符号表 ─────────────────────────────────────────
section "5. 关键内核符号 (kallsyms)"
if [ -f /proc/kallsyms ]; then
    echo "  kallsyms 可读 (无 kptr_restrict 限制)"                  | tee -a $OUT
    echo ""                                                         | tee -a $OUT

    echo "--- 断点/调试相关 ---"                                    | tee -a $OUT
    cat /proc/kallsyms | grep -w 'do_debug_exception'               | tee -a $OUT
    cat /proc/kallsyms | grep -w 'register_user_hw_breakpoint'      | tee -a $OUT
    cat /proc/kallsyms | grep -w 'unregister_hw_breakpoint'         | tee -a $OUT
    cat /proc/kallsyms | grep -w 'modify_user_hw_breakpoint'        | tee -a $OUT
    cat /proc/kallsyms | grep -w 'arch_ptrace'                      | tee -a $OUT
    echo ""                                                         | tee -a $OUT

    echo "--- Hook 辅助 ---"                                        | tee -a $OUT
    cat /proc/kallsyms | grep -w 'kallsyms_lookup_name'             | tee -a $OUT
    cat /proc/kallsyms | grep -w 'proc_root_readdir'                | tee -a $OUT
    echo ""                                                         | tee -a $OUT

    echo "--- 单步恢复 / 多核同步 ---"                              | tee -a $OUT
    cat /proc/kallsyms | grep -w 'migrate_disable'                  | tee -a $OUT
    cat /proc/kallsyms | grep -w 'migrate_enable'                   | tee -a $OUT
    cat /proc/kallsyms | grep -w 'smp_call_function'                | tee -a $OUT

    echo ""                                                         | tee -a $OUT
    echo "--- 符号总数 ---"                                         | tee -a $OUT
    echo "  kallsyms 总行数: $(wc -l < /proc/kallsyms)"             | tee -a $OUT
else
    echo "  /proc/kallsyms 不可读!"                                 | tee -a $OUT
    echo "  kptr_restrict = $(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)" | tee -a $OUT
    echo "  需要先执行: echo 0 > /proc/sys/kernel/kptr_restrict"    | tee -a $OUT
fi

# ── 第 6 组: 内存/存储信息 ──────────────────────────────────────
section "6. 内存与存储"
echo "  RAM Total : $(grep MemTotal /proc/meminfo | awk '{print $2,$3}')" | tee -a $OUT
echo "  SWAP      : $(grep SwapTotal /proc/meminfo | awk '{print $2,$3}')"  | tee -a $OUT
echo "  /data 可用: $(df -h /data | tail -1 | awk '{print $4}')"     | tee -a $OUT

# ── 第 7 组: 已加载模块 ─────────────────────────────────────────
section "7. 已加载内核模块"
lsmod 2>/dev/null | tee -a $OUT

# ── 打印每 CPU 的调试寄存器信息（如果能访问）─────────────────────
section "8. 调试寄存器信息 (如果可访问)"
echo "  注意: MSR 只能在内核态访问, 以下为尝试..."                   | tee -a $OUT
dd if=/dev/dri/card0 bs=1 count=0 2>/dev/null && echo "  /dev 可访问" | tee -a $OUT

# ── 校验 kernel 版本号, 建议下一步动作 ───────────────────────────
section "9. 下一步建议"
KVER=$(uname -r | cut -d'-' -f1)
echo "  内核版本 : $KVER"                                          | tee -a $OUT
echo "  输出文件 : $OUT"                                            | tee -a $OUT
echo ""                                                             | tee -a $OUT
echo "  PC 端执行:"                                                 | tee -a $OUT
echo "    adb pull $OUT ./"                                         | tee -a $OUT
echo ""                                                             | tee -a $OUT
echo "  然后去对应厂商开源中心下载内核源码: $KVER"                    | tee -a $OUT
echo "    小米 : https://github.com/MiCode"                         | tee -a $OUT
echo "    一加 : https://github.com/OnePlusOSS"                     | tee -a $OUT
echo "    三星 : https://opensource.samsung.com"                    | tee -a $OUT
echo "    摩托 : https://github.com/MotorolaMobilityLLC"            | tee -a $OUT
echo "    谷歌 : https://android.googlesource.com/kernel/msm"       | tee -a $OUT
echo "    LineageOS : https://github.com/LineageOS/android_kernel_" | tee -a $OUT
echo ""                                                             | tee -a $OUT
echo "============================================================" | tee -a $OUT
echo "  采集完成!"                                                   | tee -a $OUT
echo "============================================================" | tee -a $OUT
