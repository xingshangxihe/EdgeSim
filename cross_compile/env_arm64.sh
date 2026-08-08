#!/bin/bash
# =============================================================================
# EdgeSim ARM64 交叉编译环境一键安装脚本  cross_compile/env_arm64.sh
# =============================================================================
# 【脚本作用】
#   在 Ubuntu/Debian 主机上安装 aarch64-linux-gnu 交叉编译工具链，
#   并验证可正常工作。安装完成后即可用 CMake 工具链文件
#   （cross_compile/toolchain-arm64.cmake）交叉编译 EdgeSim。
#
# 【使用方法】
#   chmod +x cross_compile/env_arm64.sh
#   ./cross_compile/env_arm64.sh
#
# 【支持平台】
#   - Ubuntu 18.04 / 20.04 / 22.04 / 24.04
#   - Debian 10 / 11 / 12
#   - WSL2 (Windows Subsystem for Linux)
#
# 【设计文档对应】
#   EdgeSim_Design.md 第 4.2 节「交叉编译」
# =============================================================================


# -----------------------------------------------------------------------------
# 一、Shell 严格模式（新手必读）
# -----------------------------------------------------------------------------
# set -e : 任一命令失败（返回非 0）立即退出脚本，避免错误被忽略
# set -u : 引用未定义变量直接报错，防止变量名打错导致逻辑漏洞
# set -o pipefail : 管道中任一命令失败则整条管道失败（默认只看最后一个）
# 这三个组合是 Shell 脚本的"安全护盾"，生产脚本必须加
set -e
set -u
set -o pipefail


# -----------------------------------------------------------------------------
# 二、颜色定义（让输出更易读）
# -----------------------------------------------------------------------------
# ANSI 转义码：
#   \033[32m  : 绿色（成功信息）
#   \033[33m  : 黄色（警告信息）
#   \033[31m  : 红色（错误信息）
#   \033[0m   : 重置颜色
# echo -e 启用转义解释；-n 不换行
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
NC='\033[0m'   # No Color

# 打印函数封装
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }


# -----------------------------------------------------------------------------
# 三、检测操作系统（避免在非 Debian 系上误运行）
# -----------------------------------------------------------------------------
# /etc/os-release 是 Linux 标准发行版标识文件
# 用 source 加载它的变量（ID=ubuntu / ID=debian 等）
if [ ! -f /etc/os-release ]; then
    error "未找到 /etc/os-release，本脚本仅支持 Debian/Ubuntu 系"
fi
. /etc/os-release

info "检测到发行版: $NAME $VERSION"

# 检查 ID 是否为 debian 系（包括 ubuntu/mint 等）
case "$ID" in
    ubuntu|debian|linuxmint|pop)
        info "发行版兼容，继续安装"
        ;;
    *)
        warn "ID=$ID 非官方支持，可能无法正常安装，3 秒后继续..."
        sleep 3
        ;;
esac


# -----------------------------------------------------------------------------
# 四、检查是否为 root（必须 sudo 运行）
# -----------------------------------------------------------------------------
# apt install 需要 root 权限
# $EUID 是 bash 内置变量：当前用户的有效 UID，0=root
if [ "$EUID" -ne 0 ]; then
    error "请使用 sudo 运行：sudo ./cross_compile/env_arm64.sh"
fi


# -----------------------------------------------------------------------------
# 五、更新 apt 索引（防止缓存过期找不到包）
# -----------------------------------------------------------------------------
info "更新 apt 包索引..."
# apt-get update 拉取最新软件包列表
# 2>/dev/null 把 stderr 丢弃（避免刷屏），失败时由 set -e 终止
apt-get update -qq


# -----------------------------------------------------------------------------
# 六、安装 aarch64 交叉编译工具链
# -----------------------------------------------------------------------------
# 包名说明：
#   gcc-aarch64-linux-gnu     : C 交叉编译器（aarch64-linux-gnu-gcc）
#   g++-aarch64-linux-gnu     : C++ 交叉编译器（aarch64-linux-gnu-g++）
#   libc6-dev-arm64-cross     : 目标板 glibc 头文件与库（链接时需要）
info "安装 aarch64-linux-gnu 工具链..."
apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    libc6-dev-arm64-cross


# -----------------------------------------------------------------------------
# 七、安装 QEMU 用户模式（用于在 PC 上运行 ARM64 二进制做单元测试）
# -----------------------------------------------------------------------------
# qemu-user-static 让 aarch64 二进制能在 x86 上执行
# 用法：qemu-aarch64-static ./build/edgesim_arm64
# 注意：仅用于测试，性能远低于真机
info "安装 QEMU 用户模式（用于 ARM64 二进制本地测试）..."
apt-get install -y qemu-user-static || warn "QEMU 安装失败（可选，可跳过）"


# -----------------------------------------------------------------------------
# 八、验证工具链是否可用
# -----------------------------------------------------------------------------
# which 检查命令是否在 PATH 中
# 交叉编译器路径通常为 /usr/bin/aarch64-linux-gnu-gcc
CC_PATH=$(which aarch64-linux-gnu-gcc 2>/dev/null || true)
CXX_PATH=$(which aarch64-linux-gnu-g++ 2>/dev/null || true)

if [ -z "$CC_PATH" ]; then
    error "aarch64-linux-gnu-gcc 未找到，安装可能失败"
fi
if [ -z "$CXX_PATH" ]; then
    error "aarch64-linux-gnu-g++ 未找到，安装可能失败"
fi

info "C  编译器路径: $CC_PATH"
info "C++ 编译器路径: $CXX_PATH"


# -----------------------------------------------------------------------------
# 九、编写测试程序验证工具链
# -----------------------------------------------------------------------------
# 用 mktemp 创建临时文件，避免污染项目目录
TEST_C=$(mktemp --suffix=.c)
TEST_BIN=$(mktemp --suffix=.elf)

# cat << EOF > file 是 heredoc 写文件的标准写法
# 注意 EOF 必须顶格（前面无空格），否则 heredoc 不结束
cat << 'EOF' > "$TEST_C"
#include <stdio.h>
int main(void) {
    printf("Hello from ARM64 cross-compile!\n");
    return 0;
}
EOF

info "编译测试程序: $TEST_C"
# 用交叉编译器编译，输出 ARM64 ELF
aarch64-linux-gnu-gcc -O2 -o "$TEST_BIN" "$TEST_C"

# file 命令查看 ELF 文件架构
# 输出应包含 "ELF 64-bit LSB executable, ARM aarch64"
ARCH_INFO=$(file "$TEST_BIN" | grep -o "ARM aarch64" || true)
if [ -z "$ARCH_INFO" ]; then
    error "编译产物架构异常，请检查工具链"
fi
info "测试程序架构验证通过: $ARCH_INFO"


# -----------------------------------------------------------------------------
# 十、尝试用 QEMU 运行 ARM64 测试程序
# -----------------------------------------------------------------------------
if command -v qemu-aarch64-static >/dev/null 2>&1; then
    info "使用 QEMU 运行 ARM64 测试程序..."
    # qemu-aarch64-static -L /usr/aarch64-linux-gnu 提供动态库搜索路径
    # 否则运行时找不到 libc.so.6
    if qemu-aarch64-static -L /usr/aarch64-linux-gnu "$TEST_BIN"; then
        info "QEMU 运行成功"
    else
        warn "QEMU 运行失败（不影响交叉编译，仅测试用）"
    fi
fi


# -----------------------------------------------------------------------------
# 十一、清理临时文件
# -----------------------------------------------------------------------------
rm -f "$TEST_C" "$TEST_BIN"


# -----------------------------------------------------------------------------
# 十二、安装完成提示
# -----------------------------------------------------------------------------
echo ""
echo "============================================================"
echo -e "${GREEN}  EdgeSim ARM64 交叉编译环境安装完成${NC}"
echo "============================================================"
echo ""
echo "下一步操作："
echo "  1. 进入 EdgeSim 项目根目录"
echo "  2. 用 CMake 工具链文件交叉编译："
echo "       mkdir build-arm64 && cd build-arm64"
echo "       cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake .."
echo "       make"
echo "  3. 指定目标平台（可选）："
echo "       cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake"
echo "             -DPLATFORM=T153 ..      # 全志 T153"
echo "             -DPLATFORM=RV1106 ..    # 瑞芯微 RV1106"
echo "  4. 产物位于 build-arm64/bin/edgesim"
echo ""
echo "在 PC 上测试 ARM64 二进制（需 QEMU）："
echo "  qemu-aarch64-static -L /usr/aarch64-linux-gnu ./build-arm64/bin/edgesim"
echo ""
