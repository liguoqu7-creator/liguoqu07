#!/bin/bash
################################################################################
# 一键编译和部署 hwBreakpointProc 内核模块
#
# 用法（在 PC 上执行）:
#   chmod +x build_and_deploy.sh
#   ./build_and_deploy.sh
#
# 前提:
#   1. 手机 USB 已连接、adb 已授权、KernelSU/Magisk root 已就位
#   2. 已装 Android NDK 或 aarch64 交叉编译器
#   3. 本脚本和 hwBreakpointProc_module 目录在同一级
################################################################################

set -e

# ====================== 配置区 ======================
# Android NDK 路径（改成你的实际路径）
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
CROSS_COMPILE="$TOOLCHAIN/bin/aarch64-linux-android-"
export ARCH=arm64
export CROSS_COMPILE

# 内核源码目录（如果还没有，脚本会自动克隆）
KERNEL_SRC="$HOME/android_kernel_oneplus_sm8650"
KERNEL_REPO="https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650.git"

# 驱动模块路径（相对于本脚本）
MODULE_DIR="$(dirname "$0")/hwBreakpointProcModule/hwBreakpointProc_module"

# ====================== 第一步：从手机拉取内核配置 ======================
echo "============================================================"
echo "  第 1 步: 从手机拉取内核配置"
echo "============================================================"

adb wait-for-device
adb root 2>/dev/null || true

# 拉取 .config
adb pull /proc/config.gz /tmp/phone_config.gz 2>/dev/null || {
    echo "ERROR: 无法拉取 /proc/config.gz，检查 adb 和 root 权限"
    exit 1
}
gzip -d -f /tmp/phone_config.gz
mv /tmp/phone_config /tmp/phone_dotconfig

echo "  /proc/config.gz → /tmp/phone_dotconfig  OK"
echo "  关键配置检查:"
grep -E 'CONFIG_MODULES=|CONFIG_KPROBES=|CONFIG_KRETPROBES=|CONFIG_KALLSYMS=' /tmp/phone_dotconfig

# ====================== 第二步：获取内核源码 ======================
echo ""
echo "============================================================"
echo "  第 2 步: 获取内核源码"
echo "============================================================"

if [ ! -d "$KERNEL_SRC" ]; then
    echo "  克隆内核源码（约 2-3GB，仅第一次需要）..."
    git clone --depth=1 "$KERNEL_REPO" "$KERNEL_SRC" || {
        echo "  浅克隆失败，尝试完整克隆..."
        git clone "$KERNEL_REPO" "$KERNEL_SRC"
    }
else
    echo "  内核源码已存在: $KERNEL_SRC"
    echo "  如需更新请手动: cd $KERNEL_SRC && git pull"
fi

cd "$KERNEL_SRC"

# 找匹配的分支（Android 15 / 6.6.30）
echo "  查找匹配 Android 15 的分支..."
BRANCH=$(git branch -a | grep -i "andr.*15\|v.*15\|sun\|6.6" | head -5 || true)
if [ -n "$BRANCH" ]; then
    echo "  候选分支:"
    echo "$BRANCH"
fi

# 复制 .config
cp /tmp/phone_dotconfig "$KERNEL_SRC/.config"
echo "  .config 已复制到内核源码目录"

# ====================== 第三步：准备内核头文件 ======================
echo ""
echo "============================================================"
echo "  第 3 步: 准备内核编译环境 (modules_prepare)"
echo "============================================================"

# 确保一些必要的内核配置开启（如果手机 config 缺了）
scripts/config --enable MODULES 2>/dev/null || true
scripts/config --enable KPROBES 2>/dev/null || true
scripts/config --enable KRETPROBES 2>/dev/null || true

# modules_prepare: 生成 Module.symvers、头文件等
echo "  运行 make modules_prepare（大约 2-5 分钟）..."
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig 2>&1 | tail -3
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" modules_prepare 2>&1 | tail -5

# 检查关键文件
if [ -f "include/generated/autoconf.h" ] && [ -f "Module.symvers" ]; then
    echo "  modules_prepare 成功"
else
    echo "  WARNING: modules_prepare 可能不完整，尝试继续..."
fi

# ====================== 第四步：禁用 CFI（如果开启）======================
echo ""
echo "============================================================"
echo "  第 4 步: 处理 CFI 兼容性"
echo "============================================================"

if grep -q "CONFIG_CFI_CLANG=y" .config; then
    echo "  检测到 CONFIG_CFI_CLANG=y（你的手机开了 CFI）"
    echo "  驱动已内置 CFI 绕过（Hook __cfi_check_fn + __cfi_check_fail）"
    echo "  如果编译报 CFI 相关错，需要在内核源码中禁用:"
    echo "    sed -i 's/CONFIG_CFI_CLANG=y/# CONFIG_CFI_CLANG is not set/' .config"
    echo "  然后重新 modules_prepare"
else
    echo "  CFI 未开启，无需处理"
fi

# ====================== 第五步：编译驱动 ======================
echo ""
echo "============================================================"
echo "  第 5 步: 编译 hwBreakpointProc 驱动"
echo "============================================================"

cd "$(dirname "$0")"
MODULE_ABS="$(realpath "$MODULE_DIR")"

echo "  源码目录: $MODULE_ABS"
echo "  内核目录: $KERNEL_SRC"

# 确认 ver_control.h 配置
echo ""
echo "  === 当前 ver_control.h 配置 ==="
grep -E '#define (CONFIG_|MY_LINUX_VERSION_CODE)' "$MODULE_ABS/ver_control.h" | grep -v '^//'

echo ""
echo "  开始编译..."
make -C "$KERNEL_SRC" M="$MODULE_ABS" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" modules 2>&1

# 检查结果
if [ -f "$MODULE_ABS/hwBreakpointProc_module.ko" ]; then
    echo ""
    echo "  ============================================================"
    echo "   编译成功!"
    echo "   输出: $MODULE_ABS/hwBreakpointProc_module.ko"
    echo "  ============================================================"
else
    echo ""
    echo "  ============================================================"
    echo "   编译失败! 检查上面的错误信息"
    echo "  ============================================================"
    exit 1
fi

# ====================== 第六步：部署到手机 ======================
echo ""
echo "============================================================"
echo "  第 6 步: 部署到手机"
echo "============================================================"

adb push "$MODULE_ABS/hwBreakpointProc_module.ko" /data/local/tmp/

echo ""
echo "  加载模块..."
adb shell su -c "dmesg -c > /dev/null"  # 清空 dmesg
adb shell su -c "insmod /data/local/tmp/hwBreakpointProc_module.ko" && {
    echo "  insmod 成功!"
} || {
    echo "  insmod 失败! 查看 dmesg:"
    adb shell su -c "dmesg | tail -30"
    exit 1
}

# 检查模块是否加载
echo ""
echo "  检查模块状态..."
adb shell su -c "lsmod | grep hwBreakpoint" || echo "  (模块可能已隐藏，正常)"

# 检查 proc 节点
PROC_KEY="dce3771681d4c7a143d5d06b7d32548e"
adb shell su -c "ls /proc/$PROC_KEY/" 2>/dev/null && {
    echo "  proc 节点已创建: /proc/$PROC_KEY/"
} || {
    echo "  proc 节点未找到（可能已被隐藏，正常）"
}

# 打印内核日志
echo ""
echo "  === dmesg 关键日志 ==="
adb shell su -c "dmesg | grep -iE 'hwBreakpoint|Hello|Goodbye|kretprobe|do_debug|direct_hwbp' | tail -20"

echo ""
echo "============================================================"
echo "  全部完成!"
echo "============================================================"
echo ""
echo "  日常使用:"
echo "    adb shell su -c 'lsmod | grep hw'             # 查看模块"
echo "    adb shell su -c 'dmesg | grep hwBreakpoint'   # 查看日志"
echo "    adb shell su -c 'rmmod hwBreakpointProc_module'  # 卸载"
