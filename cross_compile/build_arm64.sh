#!/bin/bash
# =============================================================================
# EdgeSim ARM64 交叉编译一键脚本  cross_compile/build_arm64.sh
# =============================================================================
# 【脚本作用】
#   在 Ubuntu/Debian 主机上完成 EdgeSim 的 ARM64 交叉编译全流程：
#     1. 检查 aarch64 交叉编译工具链
#     2. 准备 ARM64 版 sqlite3（多架构安装 libsqlite3-dev:arm64）
#     3. 用 aarch64 工具链交叉编译 LVGL（输出到 LVGL_PATH/build-arm64，
#        避免与 x86 的 LVGL_PATH/build 冲突）
#     4. 用 CMake 工具链文件交叉编译 EdgeSim
#     5. 验证产物架构（file 应显示 ARM aarch64）
#
# 【使用方法】
#   bash cross_compile/build_arm64.sh            # 完整流程（含 LVGL）
#   bash cross_compile/build_arm64.sh --skip-lvgl  # 跳过 LVGL（纯 mock 验证工具链）
#
# 【前置条件】
#   已安装 aarch64 工具链（cross_compile/env_arm64.sh 一键安装）
#   已设置 LVGL_PATH（默认 $HOME/lvgl，可用 --lvgl-path 覆盖）
#
# 【产物】
#   build-arm64/bin/edgesim         ARM64 可执行文件
#   验证：file build-arm64/bin/edgesim  →  ELF 64-bit ... ARM aarch64
#   运行：qemu-aarch64-static -L /usr/aarch64-linux-gnu ./build-arm64/bin/edgesim
#
# 【设计说明（阶段 5）】
#   - LVGL 的静态库 liblvgl.a 必须与目标架构一致：
#     x86 编译的 liblvgl.a 链接 ARM64 时报 "Skipping incompatible file"。
#     因此这里用工具链单独编译 LVGL 到 build-arm64/，
#     并通过 -DLVGL_LIBRARY 显式传给 EdgeSim 的 CMake。
#   - sqlite3 同理需要 ARM64 库：启用多架构后安装 libsqlite3-dev:arm64。
#   - AI 引擎默认 mock 模式（不依赖 llama/whisper/onnx），保证纯交叉编译
#     链路可独立验证；真实库的 ARM64 版本按需追加。
# =============================================================================
set -e
set -u
set -o pipefail

# -----------------------------------------------------------------------------
# 常量与颜色
# -----------------------------------------------------------------------------
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# 项目根目录（脚本位于 cross_compile/ 下，上一级即根）
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN="$PROJECT_ROOT/cross_compile/toolchain-arm64.cmake"
BUILD_DIR="$PROJECT_ROOT/build-arm64"

# LVGL 路径（可用 --lvgl-path 覆盖）
LVGL_PATH="${LVGL_PATH:-$HOME/lvgl}"
# LVGL 交叉编译输出目录（与 x86 的 build/ 隔离）
LVGL_BUILD_DIR="$LVGL_PATH/build-arm64"

# 是否编译 LVGL（默认编译，--skip-lvgl 跳过）
BUILD_LVGL=1

# -----------------------------------------------------------------------------
# 参数解析
# -----------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-lvgl)
            BUILD_LVGL=0
            info "跳过 LVGL 交叉编译（纯 mock 验证）"
            ;;
        --lvgl-path)
            LVGL_PATH="$2"
            LVGL_BUILD_DIR="$LVGL_PATH/build-arm64"
            shift
            ;;
        *)
            error "未知参数: $1（支持 --skip-lvgl / --lvgl-path <路径>）"
            ;;
    esac
    shift
done


# -----------------------------------------------------------------------------
# 一、检查 aarch64 交叉编译工具链
# -----------------------------------------------------------------------------
info "===== 1. 检查 ARM64 交叉编译工具链 ====="
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || \
    error "未找到 aarch64-linux-gnu-gcc，请先安装：\n  sudo bash cross_compile/env_arm64.sh"
command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 || \
    error "未找到 aarch64-linux-gnu-g++，请先安装：\n  sudo bash cross_compile/env_arm64.sh"
info "工具链就绪: $(aarch64-linux-gnu-gcc --version | head -1)"


# -----------------------------------------------------------------------------
# 二、准备 ARM64 版 sqlite3（多架构）
# -----------------------------------------------------------------------------
info "===== 2. 检查 ARM64 sqlite3 ====="
# libsqlite3 在交叉编译时需要 ARM64 版本（/usr/aarch64-linux-gnu/lib）
# 检查 /usr/aarch64-linux-gnu 下是否有 sqlite3 头文件
if [ ! -f /usr/aarch64-linux-gnu/include/sqlite3.h ]; then
    warn "未找到 ARM64 sqlite3 头文件，尝试启用多架构并安装..."
    info "启用 arm64 架构支持（需要 sudo）..."
    sudo dpkg --add-architecture arm64
    sudo apt-get update -qq
    info "安装 libsqlite3-dev:arm64 ..."
    sudo apt-get install -y libsqlite3-dev:arm64 || \
        error "安装 libsqlite3-dev:arm64 失败，请手动处理"
else
    info "ARM64 sqlite3 已就绪"
fi


# -----------------------------------------------------------------------------
# 三、交叉编译 LVGL（输出到 build-arm64/，与 x86 build/ 隔离）
# -----------------------------------------------------------------------------
if [ "$BUILD_LVGL" -eq 1 ]; then
    info "===== 3. 交叉编译 LVGL ====="
    if [ ! -f "$LVGL_PATH/lvgl.h" ]; then
        warn "LVGL_PATH=$LVGL_PATH 下无 lvgl.h，跳过 LVGL 编译"
        warn "可用 --skip-lvgl 显式跳过，或用 --lvgl-path 指定正确路径"
        LVGL_LIB_ARG=""
    else
        info "LVGL 源码: $LVGL_PATH"
        info "输出目录: $LVGL_BUILD_DIR"

        # 用 aarch64 工具链配置 LVGL（独立构建目录）
        cmake -S "$LVGL_PATH" -B "$LVGL_BUILD_DIR" \
            -DCMAKE_TOOLCHAIN_FILE="$PROJECT_ROOT/cross_compile/toolchain-arm64.cmake" \
            -DCMAKE_BUILD_TYPE=Release || \
            error "LVGL CMake 配置失败"

        # 编译 LVGL（-j 并行）
        cmake --build "$LVGL_BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" || \
            error "LVGL 编译失败"

        # 确认产物（v8.3 在 build-arm64/lib/，v8.2 在 build-arm64/）
        LVGL_LIB_ARM64=""
        if [ -f "$LVGL_BUILD_DIR/lib/liblvgl.a" ]; then
            LVGL_LIB_ARM64="$LVGL_BUILD_DIR/lib/liblvgl.a"
        elif [ -f "$LVGL_BUILD_DIR/liblvgl.a" ]; then
            LVGL_LIB_ARM64="$LVGL_BUILD_DIR/liblvgl.a"
        fi
        if [ -z "$LVGL_LIB_ARM64" ]; then
            warn "未找到 liblvgl.a（检查 $LVGL_BUILD_DIR/lib/），将不链接 LVGL"
            LVGL_LIB_ARG=""
        else
            info "ARM64 liblvgl.a: $LVGL_LIB_ARM64"
            # 传给 EdgeSim CMake，显式指定 ARM64 LVGL 库
            LVGL_LIB_ARG="-DLVGL_LIBRARY=$LVGL_LIB_ARM64"
        fi
    fi
else
    LVGL_LIB_ARG=""
fi


# -----------------------------------------------------------------------------
# 四、交叉编译 EdgeSim
# -----------------------------------------------------------------------------
info "===== 4. 交叉编译 EdgeSim ====="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# CMake 配置：
#   -DCMAKE_TOOLCHAIN_FILE  ：aarch64 工具链（自动设置 NO_SDL/ARM64 宏）
#   -DLVGL_PATH             ：LVGL 源码根目录（头文件，平台无关）
#   -DLVGL_LIBRARY          ：ARM64 版 liblvgl.a（阶段 5 新增，避免链错 x86 库）
#   -DBUILD_EDGESIM=ON      ：链接最终 edgesim（含 UI 时必需 LVGL）
#   -DBUILD_TESTS=ON        ：同时编译各模块测试程序（QEMU 可跑）
#   AI 引擎默认 mock 模式（WITH_* 全 OFF），不依赖第三方 AI 库
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DLVGL_PATH="$LVGL_PATH" \
    $LVGL_LIB_ARG \
    -DBUILD_EDGESIM=ON \
    -DBUILD_TESTS=ON

info "开始编译（并行 -j$(nproc 2>/dev/null || echo 4)）..."
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" || \
    error "EdgeSim 交叉编译失败"


# -----------------------------------------------------------------------------
# 五、验证产物架构
# -----------------------------------------------------------------------------
info "===== 5. 验证产物 ====="
if [ -f "$BUILD_DIR/bin/edgesim" ]; then
    file "$BUILD_DIR/bin/edgesim"
    # 检查是否确为 ARM aarch64（防止意外链接了 x86 库）
    ARCH=$(file "$BUILD_DIR/bin/edgesim" | grep -o "ARM aarch64" || true)
    if [ -z "$ARCH" ]; then
        error "产物不是 ARM aarch64，请检查工具链与依赖库架构"
    fi
    info "edgesim 架构验证通过: $ARCH"
else
    warn "未生成 edgesim（可能因 LVGL 缺失），检查测试程序："
fi

# 列出全部产物
echo ""
echo "============================================================"
echo -e "${GREEN}  EdgeSim ARM64 交叉编译完成${NC}"
echo "============================================================"
echo "  产物目录    : $BUILD_DIR/bin/"
ls -la "$BUILD_DIR/bin/" 2>/dev/null || true
echo ""
echo "下一步："
echo "  在 PC 上用 QEMU 运行测试（mock 模式）:"
echo "    qemu-aarch64-static -L /usr/aarch64-linux-gnu $BUILD_DIR/bin/test_hardware_sim"
echo "  部署到开发板/安卓 Termux:"
echo "    bash scripts/termux_deploy.sh（需 adb/ssh）"
echo ""
