#!/bin/bash
# =============================================================================
# EdgeSim Termux 一键部署脚本  scripts/termux_deploy.sh
# =============================================================================
# 【脚本作用】
#   把 PC 上交叉编译好的 EdgeSim ARM64 二进制一键部署到安卓手机的 Termux 环境。
#   支持两种部署方式：
#     1. ADB 推送（推荐）：通过 USB 数据线，速度快
#     2. SSH 传输：手机与 PC 同 WiFi 时使用
#
# 【前置条件】
#   PC 端：
#     - 已用 CMake 交叉编译生成 build/bin/edgesim（ARM64 二进制）：
#         mkdir build-arm64 && cd build-arm64
#         cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake ..
#         make
#     - 已安装 adb（Android Debug Bridge）或 ssh / scp
#   手机端：
#     - 安装 Termux（F-Droid 或 GitHub releases 下载，不要用 Play Store 版）
#     - 手机开启 USB 调试（设置 → 开发者选项 → USB 调试）
#     - 或在 Termux 内启动 sshd（pkg install openssh && sshd）
#
# 【使用方法】
#   方式 1（ADB，推荐）：
#     1. 手机用 USB 连 PC，开启 USB 调试
#     2. 执行：bash scripts/termux_deploy.sh --method adb
#
#   方式 2（SSH）：
#     1. 手机 Termux 内执行 sshd，查看 IP（ifconfig）
#     2. 执行：bash scripts/termux_deploy.sh --method ssh \\
#               --host 192.168.1.100 --port 8022 --user root
#
#   方式 3（仅生成部署包，手动复制）：
#     bash scripts/termux_deploy.sh --method pack
#
# 【设计文档对应】
#   EdgeSim_Design.md 第 1.4.4 节「ARM64 Linux：适配安卓Termux」
#   EdgeSim_Design.md 第 4.3 节「scripts/termux_deploy.sh：一键部署到安卓Termux」
# =============================================================================


# -----------------------------------------------------------------------------
# 一、严格模式
# -----------------------------------------------------------------------------
set -e   # 任一命令失败立即退出
set -u   # 未定义变量报错


# -----------------------------------------------------------------------------
# 二、颜色输出
# -----------------------------------------------------------------------------
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
BLUE='\033[34m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }
step()  { echo -e "${BLUE}[STEP]${NC}  $*"; }


# -----------------------------------------------------------------------------
# 三、默认变量
# -----------------------------------------------------------------------------
# 项目根目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 部署方法：adb / ssh / pack
DEPLOY_METHOD="adb"

# SSH 参数
SSH_HOST=""
SSH_PORT="8022"   # Termux sshd 默认端口
SSH_USER="root"   # Termux 默认无密码 root

# 部署目标路径（手机 Termux 内）
# ~/edgesim 是 Termux 家目录下的 edgesim 子目录
# ~ 在 Termux 中展开为 /data/data/com.termux/files/home
REMOTE_DIR="~/edgesim"

# 待部署文件列表
# 注意：CMake 产物路径为 build/bin/edgesim（根 CMakeLists 设置
# CMAKE_RUNTIME_OUTPUT_DIRECTORY=build/bin）
LOCAL_BINARY="$PROJECT_ROOT/build/bin/edgesim"
LOCAL_MODELS_DIR="$PROJECT_ROOT/models"


# -----------------------------------------------------------------------------
# 四、参数解析
# -----------------------------------------------------------------------------
# while + case 是 Shell 标准参数解析模式
# shift 移除已处理的参数
# $# 是剩余参数个数
while [ $# -gt 0 ]; do
    case "$1" in
        --method)
            # $2 是 --method 的值
            DEPLOY_METHOD="$2"
            shift 2   # 消耗两个参数
            ;;
        --host)
            SSH_HOST="$2"
            shift 2
            ;;
        --port)
            SSH_PORT="$2"
            shift 2
            ;;
        --user)
            SSH_USER="$2"
            shift 2
            ;;
        --remote-dir)
            REMOTE_DIR="$2"
            shift 2
            ;;
        --help|-h)
            cat << 'EOF'
EdgeSim Termux 一键部署脚本

用法：
  bash scripts/termux_deploy.sh [选项]

选项：
  --method <adb|ssh|pack>   部署方法（默认 adb）
  --host <ip>               SSH 主机 IP（method=ssh 时必填）
  --port <port>             SSH 端口（默认 8022，Termux sshd 默认）
  --user <user>             SSH 用户（默认 root）
  --remote-dir <path>       远程部署目录（默认 ~/edgesim）
  --help                    显示本帮助

示例：
  # ADB 部署（手机 USB 连接）
  bash scripts/termux_deploy.sh --method adb

  # SSH 部署（同 WiFi）
  bash scripts/termux_deploy.sh --method ssh --host 192.168.1.100

  # 仅打包，手动复制
  bash scripts/termux_deploy.sh --method pack
EOF
            exit 0
            ;;
        *)
            error "未知参数: $1
使用 --help 查看用法"
            ;;
    esac
done


# -----------------------------------------------------------------------------
# 五、校验前置条件
# -----------------------------------------------------------------------------
step "1. 校验前置条件"

# 5.1 检查 build/bin/edgesim 是否存在
if [ ! -f "$LOCAL_BINARY" ]; then
    error "可执行文件不存在: $LOCAL_BINARY
请先用 CMake 交叉编译：
  mkdir build-arm64 && cd build-arm64
  cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake ..
  make"
fi

# 5.2 检查架构是否为 ARM64
ARCH_INFO=$(file "$LOCAL_BINARY" | grep -o "ARM aarch64" || true)
if [ -z "$ARCH_INFO" ]; then
    warn "可执行文件非 ARM64 架构，部署到 Termux 可能无法运行"
    file "$LOCAL_BINARY"
fi
info "可执行文件: $LOCAL_BINARY"
info "部署方法  : $DEPLOY_METHOD"
info "远程目录  : $REMOTE_DIR"


# -----------------------------------------------------------------------------
# 六、生成启动脚本（手机端使用）
# -----------------------------------------------------------------------------
step "2. 生成手机端启动脚本"

# 启动脚本会被一起部署到手机，简化运行流程
# 放在 build/edgesim_termux/ 内统一打包
PACK_DIR="$PROJECT_ROOT/build/edgesim_termux"
rm -rf "$PACK_DIR"
mkdir -p "$PACK_DIR"

# 复制可执行文件
cp "$LOCAL_BINARY" "$PACK_DIR/"

# 复制依赖库（如果有 build/libs/ 目录，由 termux_build.sh 生成）
if [ -d "$PROJECT_ROOT/build/libs" ]; then
    mkdir -p "$PACK_DIR/libs"
    cp "$PROJECT_ROOT/build/libs/"* "$PACK_DIR/libs/" 2>/dev/null || true
    info "已复制依赖库到打包目录"
fi

# 复制模型文件（如果有）
if [ -d "$LOCAL_MODELS_DIR" ]; then
    mkdir -p "$PACK_DIR/models"
    # 模型文件可能很大，仅复制小于 2GB 的文件
    # find -size -2G 找出小于 2GB 的文件
    info "复制模型文件（小于 2GB）..."
    find "$LOCAL_MODELS_DIR" -type f -size -2G -exec cp {} "$PACK_DIR/models/" \; 2>/dev/null || true
fi

# 生成 run_edgesim.sh 启动脚本
cat > "$PACK_DIR/run_edgesim.sh" << 'EOF'
#!/data/data/com.termux/files/usr/bin/bash
# EdgeSim Termux 启动脚本
# 部署后执行：bash ~/edgesim/run_edgesim.sh

# 获取脚本所在目录（绝对路径）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# 设置动态库搜索路径
# $SCRIPT_DIR/libs    : 自带的依赖库
# $PREFIX/lib         : Termux 系统库
export LD_LIBRARY_PATH="$SCRIPT_DIR/libs:$PREFIX/lib:${LD_LIBRARY_PATH:-}"

# 设置数据目录
export EDGESIM_DATA_DIR="$SCRIPT_DIR/data"
mkdir -p "$EDGESIM_DATA_DIR"

# 防止系统杀进程（Termux 后台运行需要）
# termux-wake-lock 防止 Android 系统休眠时杀掉 EdgeSim
if command -v termux-wake-lock >/dev/null 2>&1; then
    termux-wake-lock
    echo "[INFO] 已申请 wake-lock（防止系统休眠杀进程）"
fi

# 启动 EdgeSim
# exec 让 edgesim 替换当前 shell，接收信号更可靠
exec "$SCRIPT_DIR/edgesim" "$@"
EOF

chmod +x "$PACK_DIR/run_edgesim.sh"
info "启动脚本已生成: run_edgesim.sh"

# 生成安装脚本（部署到手机后自动执行）
cat > "$PACK_DIR/install.sh" << 'EOF'
#!/data/data/com.termux/files/usr/bin/bash
# EdgeSim Termux 安装脚本
# 在手机 Termux 内执行：bash ~/edgesim/install.sh

set -e

INSTALL_DIR="${HOME}/edgesim"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "[INFO] 安装目录: $INSTALL_DIR"

# 创建安装目录
mkdir -p "$INSTALL_DIR"

# 复制所有文件
cp -r "$SCRIPT_DIR"/* "$INSTALL_DIR/"

# 设置可执行权限
chmod +x "$INSTALL_DIR/edgesim"
chmod +x "$INSTALL_DIR/run_edgesim.sh"

# 创建桌面图标（可选，需要 Termux:Widget）
DESKTOP_DIR="${HOME}/.shortcuts"
mkdir -p "$DESKTOP_DIR"
cat > "$DESKTOP_DIR/EdgeSim.sh" << 'ICON'
#!/data/data/com.termux/files/usr/bin/bash
bash ~/edgesim/run_edgesim.sh
ICON
chmod +x "$DESKTOP_DIR/EdgeSim.sh"

echo "[INFO] 安装完成"
echo "[INFO] 启动方法："
echo "       bash ~/edgesim/run_edgesim.sh"
echo "[INFO] 或在桌面 Termux:Widget 中点击 EdgeSim 图标"
EOF

chmod +x "$PACK_DIR/install.sh"
info "安装脚本已生成: install.sh"


# -----------------------------------------------------------------------------
# 七、根据部署方法执行
# -----------------------------------------------------------------------------

# ----- 方法 1：ADB 部署 -----
deploy_via_adb() {
    step "3. 通过 ADB 部署"

    # 检查 adb 命令
    if ! command -v adb >/dev/null 2>&1; then
        error "未找到 adb 命令
请安装：
  Ubuntu: sudo apt install adb
  Windows: 从 https://developer.android.com/studio 下载 Platform Tools"
    fi

    # 检查设备是否连接
    # adb devices 列出已连接设备
    info "检测已连接设备..."
    DEVICES=$(adb devices | grep -E "^[^\t]+\tdevice$" | awk '{print $1}')
    if [ -z "$DEVICES" ]; then
        error "未检测到设备
请检查：
  1. 手机已通过 USB 连接 PC
  2. 手机已开启 USB 调试（设置 → 开发者选项）
  3. 手机上已确认 USB 调试授权弹窗"
    fi
    info "已连接设备: $DEVICES"

    # 推送文件到手机
    # adb push <local> <remote>
    # Termux 家目录路径：/data/data/com.termux/files/home/
    # 简写为 /sdcard/edgesim_termux/ 临时目录，再让用户在 Termux 内移动
    REMOTE_TMP="/sdcard/edgesim_termux"

    info "推送文件到手机 $REMOTE_TMP ..."
    adb shell "rm -rf $REMOTE_TMP" 2>/dev/null || true
    adb shell "mkdir -p $REMOTE_TMP"
    adb push "$PACK_DIR" "$REMOTE_TMP/"

    info "文件已推送至手机: $REMOTE_TMP"
    echo ""
    echo "============================================================"
    echo -e "${GREEN}  ADB 部署完成${NC}"
    echo "============================================================"
    echo ""
    echo "在手机 Termux 内执行以下命令完成安装："
    echo "  cd /sdcard/edgesim_termux"
    echo "  bash install.sh"
    echo ""
    echo "启动 EdgeSim："
    echo "  bash ~/edgesim/run_edgesim.sh"
    echo ""
}


# ----- 方法 2：SSH 部署 -----
deploy_via_ssh() {
    step "3. 通过 SSH 部署"

    # 检查参数
    if [ -z "$SSH_HOST" ]; then
        error "SSH 部署需要 --host 参数
示例：bash scripts/termux_deploy.sh --method ssh --host 192.168.1.100"
    fi

    # 检查 scp / ssh 命令
    if ! command -v scp >/dev/null 2>&1; then
        error "未找到 scp 命令，请安装 openssh-client"
    fi

    # 测试 SSH 连接
    info "测试 SSH 连接: $SSH_USER@$SSH_HOST:$SSH_PORT"
    # -p 指定端口，-o BatchMode=yes 禁止交互式密码输入
    # -o ConnectTimeout=5 5 秒超时
    if ! ssh -p "$SSH_PORT" -o BatchMode=yes -o ConnectTimeout=5 \
            "$SSH_USER@$SSH_HOST" "echo ok" >/dev/null 2>&1; then
        error "SSH 连接失败
请检查：
  1. 手机 Termux 已启动 sshd（pkg install openssh && sshd）
  2. 手机 IP 与端口正确（Termux 默认 8022）
  3. 已配置 SSH 密钥免密登录
     （在 PC 执行：ssh-copy-id -p $SSH_PORT $SSH_USER@$SSH_HOST）"
    fi
    info "SSH 连接正常"

    # 在远程创建目录
    info "创建远程目录: $REMOTE_DIR"
    ssh -p "$SSH_PORT" "$SSH_USER@$SSH_HOST" "mkdir -p $REMOTE_DIR"

    # 用 scp 推送整个打包目录
    # -r 递归复制目录
    # -P 注意大写 P 是 scp 的端口参数（与 ssh 的 -p 不同）
    info "推送文件到手机..."
    scp -P "$SSH_PORT" -r "$PACK_DIR"/* "$SSH_USER@$SSH_HOST:$REMOTE_DIR/"

    info "文件已推送至手机: $REMOTE_DIR"
    echo ""
    echo "============================================================"
    echo -e "${GREEN}  SSH 部署完成${NC}"
    echo "============================================================"
    echo ""
    echo "在手机 Termux 内执行以下命令完成安装："
    echo "  cd $REMOTE_DIR"
    echo "  bash install.sh"
    echo ""
    echo "启动 EdgeSim："
    echo "  bash ~/edgesim/run_edgesim.sh"
    echo ""
}


# ----- 方法 3：仅打包 -----
deploy_via_pack() {
    step "3. 仅生成部署包"

    # 打包为 tar.gz 方便手动复制
    PACK_TAR="$PROJECT_ROOT/build/edgesim_termux.tar.gz"
    info "打包为: $PACK_TAR"

    # tar czf 创建 gzip 压缩包
    # -C 切换到指定目录后再打包（避免路径前缀）
    tar czf "$PACK_TAR" -C "$PROJECT_ROOT/build" edgesim_termux

    PACK_SIZE=$(du -h "$PACK_TAR" | cut -f1)
    info "打包完成: $PACK_TAR ($PACK_SIZE)"
    echo ""
    echo "============================================================"
    echo -e "${GREEN}  部署包生成完成${NC}"
    echo "============================================================"
    echo ""
    echo "手动部署方法："
    echo "  1. 把 $PACK_TAR 复制到手机（USB / 网盘 / 邮件 / 蓝牙）"
    echo "  2. 在 Termux 内执行："
    echo "       cd ~"
    echo "       tar xzf /sdcard/edgesim_termux.tar.gz"
    echo "       cd edgesim_termux"
    echo "       bash install.sh"
    echo ""
    echo "启动 EdgeSim："
    echo "  bash ~/edgesim/run_edgesim.sh"
    echo ""
}


# -----------------------------------------------------------------------------
# 八、分发执行
# -----------------------------------------------------------------------------
case "$DEPLOY_METHOD" in
    adb)
        deploy_via_adb
        ;;
    ssh)
        deploy_via_ssh
        ;;
    pack)
        deploy_via_pack
        ;;
    *)
        error "未知部署方法: $DEPLOY_METHOD
可选值：adb / ssh / pack"
        ;;
esac
