#!/data/data/com.termux/files/usr/bin/bash
# =============================================================================
# EdgeSim Termux 一键编译脚本  cross_compile/termux_build.sh
# =============================================================================
# 【脚本作用】
#   在安卓手机 Termux 应用内一键编译 EdgeSim ARM64 版本。
#   Termux 自带 clang 编译器，无需交叉工具链，直接在手机上原生编译。
#
# 【使用方法】
#   1. 安卓手机安装 Termux（F-Droid 或 GitHub releases 下载）
#   2. 启动 Termux，执行以下命令：
#        pkg update && pkg install git clang make cmake
#        git clone https://github.com/<your>/EdgeSim.git
#        cd EdgeSim
#        bash cross_compile/termux_build.sh
#
# 【Termux 与 Linux 的差异】
#   - 路径前缀：/data/data/com.termux/files/usr/（非 /usr/）
#   - 库路径：  $PREFIX/lib（非 /usr/lib）
#   - 头文件：  $PREFIX/include
#   - 无 systemd / 无 root（默认 unprivileged）
#   - 无 SDL2 桌面（强制 NO_SDL=1）
#   - 无 libsqlite3-dev 需用 pkg install sqlite 安装
#
# 【设计文档对应】
#   EdgeSim_Design.md 第 1.4.4 节「ARM64 Linux：适配安卓 Termux」
#   EdgeSim_Design.md 第 4.2 节「条件编译：ARM64 下禁用 SDL2」
# =============================================================================


# -----------------------------------------------------------------------------
# 一、严格模式与错误处理
# -----------------------------------------------------------------------------
set -e   # 任一命令失败立即退出
set -u   # 未定义变量报错


# -----------------------------------------------------------------------------
# 二、颜色输出
# -----------------------------------------------------------------------------
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }


# -----------------------------------------------------------------------------
# 三、检测是否在 Termux 环境
# -----------------------------------------------------------------------------
# Termux 的 PREFIX 环境变量固定为 /data/data/com.termux/files/usr
# 检测此变量是否存在，确认运行环境
if [ -z "${PREFIX:-}" ] || [ ! -d "$PREFIX" ]; then
    error "未检测到 Termux 环境（PREFIX 变量缺失）
请确认本脚本在 Termux App 内运行，而非普通 Linux"
fi
info "检测到 Termux 环境: $PREFIX"


# -----------------------------------------------------------------------------
# 四、检查必需的命令是否已安装
# -----------------------------------------------------------------------------
# command -v <cmd> 等价于 which，返回命令路径或失败
# 缺失则提示用户安装并退出
check_cmd() {
    local cmd=$1
    local pkg=$2
    if ! command -v "$cmd" >/dev/null 2>&1; then
        error "缺少命令: $cmd
请先安装：pkg install $pkg"
    fi
}

# 项目使用 CMake 构建，所需工具如下：
#   - cmake  ：CMake 配置工具（生成构建文件）
#   - make   ：执行 CMake 生成的构建文件
#   - clang  ：Termux 默认编译器（CMake 自动检测）
#   - git    ：拉取源码
check_cmd make    make
check_cmd cmake   cmake
check_cmd clang   clang
check_cmd git     git

info "编译工具检查通过"


# -----------------------------------------------------------------------------
# 五、检查 EdgeSim 项目根目录
# -----------------------------------------------------------------------------
# 脚本应在项目根目录运行，检测根 CMakeLists.txt 是否存在
if [ ! -f ./CMakeLists.txt ]; then
    error "未找到根目录 CMakeLists.txt，请在 EdgeSim 项目根目录运行本脚本"
fi
if [ ! -d ./business ] || [ ! -d ./ai_engine ] || [ ! -d ./ui_lvgl ]; then
    error "目录结构不完整，缺少 business/ai_engine/ui_lvgl"
fi
info "EdgeSim 项目结构验证通过"


# -----------------------------------------------------------------------------
# 六、安装依赖（sqlite3 / pthread / cmake）
# -----------------------------------------------------------------------------
# Termux 用 pkg 而非 apt-get，包名略有差异
# sqlite      : SQLite3 库（business/sqlite_db 依赖）
# clang       : 编译器
# make        : 构建工具（执行 CMake 生成的构建文件）
# cmake       : CMake 配置工具
# libc++      : C++ 标准库（clang 用 libc++ 而非 libstdc++）
info "安装编译依赖..."
pkg install -y sqlite clang make cmake libc++ || warn "部分依赖安装失败，可能需手动处理"


# -----------------------------------------------------------------------------
# 七、CMake 配置参数
# -----------------------------------------------------------------------------
# 参数说明：
#   -DNO_SDL=ON                    ：Termux 无桌面，禁用 SDL2 依赖
#   -DBUILD_EDGESIM=ON             ：链接最终 edgesim 可执行文件
#   -DLVGL_PATH=<path>             ：可选。Termux 需显示界面时指定 LVGL 根目录
#   -DCMAKE_C_FLAGS/CXX_FLAGS      ：追加 Termux 专用宏与头文件路径
#                                    （-DTERMUX 让源码检测 Termux 环境，
#                                     -I$PREFIX/include 让编译器找 sqlite3.h）
#   -DCMAKE_EXE_LINKER_FLAGS       ：-L$PREFIX/lib 让链接器找 libsqlite3.so，
#                                    -lc++_shared 使用 clang 的 C++ 共享库（减小体积）
# 注意：这些 -DCMAKE_*_FLAGS 会与根 CMakeLists.txt 中的全局 CFLAGS 自动拼接，
#       不会相互覆盖。

# 若设置了 LVGL_PATH 环境变量且目录有效，则传给 CMake
LVGL_ARG=""
if [ -n "${LVGL_PATH:-}" ] && [ -d "$LVGL_PATH" ]; then
    LVGL_ARG="-DLVGL_PATH=$LVGL_PATH"
    info "启用 LVGL 界面: $LVGL_PATH"
else
    warn "未设置 LVGL_PATH，将不编译 ui_lvgl（无界面）"
fi

# 汇总所有 CMake 命令行参数（bash 数组，逐项传给 cmake）
CMAKE_DEFS=(
    -DNO_SDL=ON
    -DBUILD_EDGESIM=ON
    "-DCMAKE_C_FLAGS=-DTERMUX -I$PREFIX/include"
    "-DCMAKE_CXX_FLAGS=-DTERMUX -I$PREFIX/include"
    "-DCMAKE_EXE_LINKER_FLAGS=-L$PREFIX/lib -lc++_shared"
)
# LVGL 参数单独追加：仅在设置了有效 LVGL_PATH 时才加入数组，
# 避免传空字符串参数给 cmake
if [ -n "$LVGL_ARG" ]; then
    CMAKE_DEFS+=("$LVGL_ARG")
fi

info "CMake 参数: ${CMAKE_DEFS[*]}"


# -----------------------------------------------------------------------------
# 八、清理旧构建产物
# -----------------------------------------------------------------------------
info "清理旧构建产物..."
rm -rf build


# -----------------------------------------------------------------------------
# 九、CMake 配置 + 编译
# -----------------------------------------------------------------------------
# Termux 是原生 aarch64 环境，不需要交叉工具链，CMake 自动检测 clang
# 第一步：cmake -B build 配置（生成构建文件）
# 第二步：cmake --build build 编译（内部自动调用 make -j）
info "CMake 配置..."
cmake -B build "${CMAKE_DEFS[@]}" .
info "开始编译 EdgeSim Termux 版本..."
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"

# 验证编译产物
# 注意：CMake 的 CMAKE_RUNTIME_OUTPUT_DIRECTORY 把可执行文件输出到 build/bin/ 下
# （根 CMakeLists.txt 设置）。
if [ ! -f ./build/bin/edgesim ]; then
    error "编译失败：build/bin/edgesim 不存在"
fi

# file 显示架构信息，应包含 "ARM aarch64"
info "编译产物信息："
file ./build/bin/edgesim


# -----------------------------------------------------------------------------
# 十、复制依赖库到 build 目录（方便部署）
# -----------------------------------------------------------------------------
# Termux 的可执行文件依赖 $PREFIX/lib 下的 .so 文件
# 部署到其他手机时这些库可能不存在，所以一并打包
info "复制运行时依赖库到 build/ 目录..."
mkdir -p build/libs

# 用 ldd 列出依赖，逐个复制
# Termux 的 ldd 是脚本，输出格式与 GNU ldd 略有不同
# 这里直接复制常用库（保守做法）
for lib in libsqlite3.so libc++_shared.so; do
    if [ -f "$PREFIX/lib/$lib" ]; then
        cp "$PREFIX/lib/$lib" build/libs/
        info "已复制: $lib"
    fi
done


# -----------------------------------------------------------------------------
# 十一、生成启动脚本（设置 LD_LIBRARY_PATH）
# -----------------------------------------------------------------------------
# Termux 部署到其他手机时，可执行文件找不到 .so
# 启动脚本设置 LD_LIBRARY_PATH 指向同目录的 libs/
info "生成启动脚本 build/run_edgesim.sh..."
cat > build/run_edgesim.sh << 'EOF'
#!/data/data/com.termux/files/usr/bin/bash
# EdgeSim Termux 启动脚本
# 自动设置库搜索路径，无需手动 export

# 获取脚本所在目录（绝对路径）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# 设置动态库搜索路径
# $SCRIPT_DIR/libs    : 自带的依赖库
# $PREFIX/lib         : Termux 系统库（fallback）
export LD_LIBRARY_PATH="$SCRIPT_DIR/libs:$PREFIX/lib:${LD_LIBRARY_PATH:-}"

# 设置数据目录（模型文件、数据库等）
export EDGESIM_DATA_DIR="$SCRIPT_DIR/data"
mkdir -p "$EDGESIM_DATA_DIR"

# 启动 EdgeSim
# 可执行文件位于 build/bin/ 下（CMake 产物路径）
exec "$SCRIPT_DIR/bin/edgesim" "$@"
EOF

# chmod +x 让脚本可执行
chmod +x build/run_edgesim.sh


# -----------------------------------------------------------------------------
# 十二、编译完成提示
# -----------------------------------------------------------------------------
echo ""
echo "============================================================"
echo -e "${GREEN}  EdgeSim Termux 编译完成${NC}"
echo "============================================================"
echo ""
echo "产物位置："
echo "  build/bin/edgesim      可执行文件"
echo "  build/libs/            依赖库（部署到其他手机用）"
echo "  build/run_edgesim.sh   启动脚本"
echo ""
echo "运行方法："
echo "  bash build/run_edgesim.sh"
echo ""
echo "部署到其他手机："
echo "  1. 把整个 build/ 目录复制到目标手机 Termux"
echo "  2. 在目标手机执行：bash build/run_edgesim.sh"
echo ""
echo "提示：Termux 后台运行请用 termux-wake-lock 防止系统杀进程"
echo ""
