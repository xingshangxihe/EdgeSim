#!/bin/bash
# =============================================================================
# EdgeSim AppImage 打包脚本  scripts/build_appimage.sh
# =============================================================================
# 【脚本作用】
#   把 build/edgesim 可执行文件打包为 AppImage 单文件，
#   用户双击即可运行，无需安装依赖（除 glibc 外）。
#
# 【AppImage 原理】
#   AppImage 是一个 SquashFS 文件系统镜像，附带运行时挂载逻辑：
#   1. 用户双击 .AppImage 文件
#   2. 内嵌的运行时（runtime）把镜像挂载到 /tmp/.mount_xxx/
#   3. 执行镜像内的 AppRun 启动脚本
#   4. 程序退出后自动卸载
#   优点：单文件分发，无需 root，跨发行版兼容。
#
# 【使用方法】
#   1. 先用 CMake 构建生成 build/bin/edgesim（阶段 0 起唯一构建方式）：
#        mkdir build && cd build && cmake .. -DLVGL_PATH=... && make
#   2. 直接运行本脚本：
#        bash scripts/build_appimage.sh
#   3. 输出：build/EdgeSim-x86_64.AppImage
#
# 【前置依赖】
#   - build/edgesim 可执行文件已存在
#   - wget：下载 appimagetool
#   - file：校验架构
#
# 【设计文档对应】
#   EdgeSim_Design.md 第 1.4.4 节「x86 Linux：打包AppImage单文件」
#   EdgeSim_Design.md 第 4.3 节「scripts/build_appimage.sh」
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
# 三、变量定义
# -----------------------------------------------------------------------------
# 项目根目录：脚本所在目录的上一级（scripts/.. ）
# $(dirname "$0") 取脚本所在目录，cd 进入后 pwd 取绝对路径
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 可执行文件路径（CMake 产物，CMAKE_RUNTIME_OUTPUT_DIRECTORY=build/bin）
APP_BINARY="$PROJECT_ROOT/build/bin/edgesim"

# AppImage 工作目录（临时构建目录）
APPDIR="$PROJECT_ROOT/build/EdgeSim.AppDir"

# AppImage 输出文件名（带架构标识）
OUTPUT_APPIMAGE="$PROJECT_ROOT/build/EdgeSim-x86_64.AppImage"

# appimagetool 路径（用于把 AppDir 打包为 .AppImage）
APPIMAGETOOL="$PROJECT_ROOT/build/appimagetool-x86_64.AppImage"

# appimagetool 下载地址（GitHub releases，持续更新）
APPIMAGETOOL_URL="https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"

# 应用元数据
APP_NAME="EdgeSim"
APP_VERSION="1.0.0"
APP_ICON="edgesim"   # 图标名（不带扩展名）


# -----------------------------------------------------------------------------
# 四、校验前置条件
# -----------------------------------------------------------------------------
step "1. 校验前置条件"

# 4.1 检查 build/bin/edgesim 是否存在
if [ ! -f "$APP_BINARY" ]; then
    error "可执行文件不存在: $APP_BINARY
请先用 CMake 构建：
  mkdir build && cd build && cmake .. -DLVGL_PATH=/path/to/lvgl && make"
fi

# 4.2 检查架构是否为 x86_64（AppImage 主要面向 x86_64）
# file 命令查看 ELF 头，grep 匹配架构
ARCH_INFO=$(file "$APP_BINARY" | grep -o "x86-64" || true)
if [ -z "$ARCH_INFO" ]; then
    warn "可执行文件非 x86-64，AppImage 可能无法在桌面运行"
    warn "如果是 ARM64 交叉编译产物，请用 termux_deploy.sh 部署"
fi
info "可执行文件: $APP_BINARY"

# 4.3 检查必需命令
for cmd in wget file chmod; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        error "缺少命令: $cmd，请安装"
    fi
done
info "必需命令检查通过"


# -----------------------------------------------------------------------------
# 五、准备 AppDir 目录结构
# -----------------------------------------------------------------------------
step "2. 准备 AppDir 目录结构"

# AppImage 标准目录结构：
#   EdgeSim.AppDir/
#   ├── AppRun          启动脚本（可执行）
#   ├── edgesim.desktop 桌面入口文件
#   ├── edgesim.png     应用图标
#   └── usr/
#       ├── bin/        可执行文件
#       ├── lib/        依赖库
#       └── share/      资源文件（模型、字体等）

# 清理旧目录
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/lib"
mkdir -p "$APPDIR/usr/share/edgesim/models"
mkdir -p "$APPDIR/usr/share/edgesim/data"

info "AppDir 已创建: $APPDIR"


# -----------------------------------------------------------------------------
# 六、复制可执行文件与依赖库
# -----------------------------------------------------------------------------
step "3. 复制可执行文件与依赖库"

# 6.1 复制主可执行文件
cp "$APP_BINARY" "$APPDIR/usr/bin/"
chmod +x "$APPDIR/usr/bin/edgesim"
info "已复制: usr/bin/edgesim"

# 6.2 复制运行时依赖库（ldd 列出依赖，过滤出 /usr/lib 下的）
# ldd 输出格式：libxxx.so => /usr/lib/x86_64-linux-gnu/libxxx.so (0x...)
# awk 提取第 3 字段（库路径），grep 过滤系统库
info "分析动态库依赖..."
ldd "$APP_BINARY" 2>/dev/null | awk '{print $3}' | grep -E '^/usr/lib' | while read -r lib; do
    if [ -f "$lib" ]; then
        cp "$lib" "$APPDIR/usr/lib/" 2>/dev/null || true
        info "  已复制库: $(basename "$lib")"
    fi
done


# -----------------------------------------------------------------------------
# 七、生成桌面入口文件 .desktop
# -----------------------------------------------------------------------------
step "4. 生成 .desktop 桌面入口文件"

# .desktop 文件是 Linux 桌面环境标准的应用入口配置
# 字段说明：
#   [Desktop Entry]     ：固定段头
#   Type=Application    ：类型为应用
#   Name=               ：应用名称
#   Exec=               ：启动命令（AppRun 内会包装）
#   Icon=               ：图标名（不带扩展名，对应 .png 文件）
#   Categories=         ：菜单分类
#   Comment=            ：鼠标悬停提示
#   Terminal=false      ：不在终端中运行
cat > "$APPDIR/edgesim.desktop" << EOF
[Desktop Entry]
Type=Application
Name=EdgeSim
Name[zh_CN]=EdgeSim 离线AI助手
Exec=edgesim
Icon=$APP_ICON
Categories=Office;Utility;ArtificialIntelligence;
Comment=Offline AI assistant with embedded hardware simulator
Comment[zh_CN]=离线AI助手 + 嵌入式硬件仿真器
Terminal=false
StartupNotify=true
EOF
info "已生成: edgesim.desktop"


# -----------------------------------------------------------------------------
# 八、生成 AppRun 启动脚本
# -----------------------------------------------------------------------------
step "5. 生成 AppRun 启动脚本"

# AppRun 是 AppImage 的入口，由 runtime 在挂载后调用
# 它必须可执行（chmod +x）
# 关键变量：
#   APPDIR   ：AppImage 运行时设置的变量，指向挂载点（即 AppDir 根）
#   LD_LIBRARY_PATH：动态库搜索路径，需包含 $APPDIR/usr/lib
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
# EdgeSim AppImage 启动脚本
# 由 build_appimage.sh 生成，请勿手动修改

# APPDIR 是 AppImage runtime 自动设置的变量，指向挂载点根目录
# 若未设置（直接运行 AppDir），则用脚本所在目录
if [ -z "${APPDIR:-}" ]; then
    APPDIR="$(cd "$(dirname "$0")" && pwd)"
fi

# 设置动态库搜索路径（优先用 AppImage 内自带的库）
# $APPDIR/usr/lib 是我们打包的依赖库
# $LD_LIBRARY_PATH 保留用户原有设置
export LD_LIBRARY_PATH="$APPDIR/usr/lib:${LD_LIBRARY_PATH:-}"

# 设置数据目录（模型文件、数据库等）
# 优先用用户家目录的 .edgesim，不存在则用 AppImage 内的默认数据
export EDGESIM_DATA_DIR="${EDGESIM_DATA_DIR:-$HOME/.edgesim}"
mkdir -p "$EDGESIM_DATA_DIR"

# 如果用户家目录没模型，复制 AppImage 内的默认模型过去
if [ ! -d "$EDGESIM_DATA_DIR/models" ] && [ -d "$APPDIR/usr/share/edgesim/models" ]; then
    cp -r "$APPDIR/usr/share/edgesim/models" "$EDGESIM_DATA_DIR/"
fi

# 执行主程序，所有命令行参数透传
exec "$APPDIR/usr/bin/edgesim" "$@"
EOF

chmod +x "$APPDIR/AppRun"
info "已生成: AppRun"


# -----------------------------------------------------------------------------
# 九、生成应用图标（占位 PNG）
# -----------------------------------------------------------------------------
step "6. 生成应用图标"

# 真实项目应准备 256x256 PNG 图标文件
# 这里用一个简单的占位图标（1x1 蓝色像素），方便测试流程
# 真实使用时把 edgesim.png 放到 $APPDIR/ 下覆盖即可
ICON_FILE="$APPDIR/${APP_ICON}.png"
if [ ! -f "$ICON_FILE" ]; then
    # 用 Python 生成 64x64 蓝色 PNG 占位
    python3 -c "
import struct, zlib
def create_png(path, w, h, r, g, b):
    # PNG 头
    header = b'\\x89PNG\\r\\n\\x1a\\n'
    # IHDR
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    ihdr_chunk = b'IHDR' + ihdr
    ihdr_crc = zlib.crc32(ihdr_chunk)
    ihdr_full = struct.pack('>I', 13) + ihdr_chunk + struct.pack('>I', ihdr_crc)
    # IDAT
    raw = b''
    for _ in range(h):
        raw += b'\\x00' + bytes([r, g, b]) * w
    compressed = zlib.compress(raw)
    idat_chunk = b'IDAT' + compressed
    idat_crc = zlib.crc32(idat_chunk)
    idat_full = struct.pack('>I', len(compressed)) + idat_chunk + struct.pack('>I', idat_crc)
    # IEND
    iend_full = struct.pack('>I', 0) + b'IEND' + struct.pack('>I', zlib.crc32(b'IEND'))
    with open(path, 'wb') as f:
        f.write(header + ihdr_full + idat_full + iend_full)
create_png('$ICON_FILE', 64, 64, 0x33, 0x66, 0xCC)
" 2>/dev/null || warn "图标生成失败，将无图标显示"
    info "已生成占位图标: ${APP_ICON}.png (64x64)"
fi


# -----------------------------------------------------------------------------
# 十、下载 appimagetool
# -----------------------------------------------------------------------------
step "7. 下载 appimagetool"

# appimagetool 是官方工具，把 AppDir 打包成 .AppImage
# 持续更新版本（continuous）始终最新
if [ ! -f "$APPIMAGETOOL" ]; then
    info "下载 appimagetool..."
    info "URL: $APPIMAGETOOL_URL"
    wget -q -O "$APPIMAGETOOL" "$APPIMAGETOOL_URL" || error "下载失败"
    chmod +x "$APPIMAGETOOL"
    info "下载完成: $APPIMAGETOOL"
else
    info "appimagetool 已存在，跳过下载"
fi


# -----------------------------------------------------------------------------
# 十一、打包生成 AppImage
# -----------------------------------------------------------------------------
step "8. 打包生成 AppImage"

# appimagetool 调用语法：
#   appimagetool <AppDir> <output.AppImage>
# 环境变量 ARCH=x86_64 指定目标架构
info "执行 appimagetool..."
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT_APPIMAGE" --no-appstream 2>&1 || {
    warn "appimagetool 直接运行失败，尝试用 --appimage-extract-and-run 模式"
    # 某些系统 FUSE 不可用时用此模式
    "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$OUTPUT_APPIMAGE" --no-appstream
}

# 校验产物
if [ ! -f "$OUTPUT_APPIMAGE" ]; then
    error "AppImage 生成失败"
fi

# 添加可执行权限（有些系统下载后默认不可执行）
chmod +x "$OUTPUT_APPIMAGE"

# 显示产物信息
OUTPUT_SIZE=$(du -h "$OUTPUT_APPIMAGE" | cut -f1)
info "AppImage 生成成功: $OUTPUT_APPIMAGE ($OUTPUT_SIZE)"


# -----------------------------------------------------------------------------
# 十二、清理临时目录
# -----------------------------------------------------------------------------
step "9. 清理临时目录"

# 保留 AppDir 以便调试可注释下一行
rm -rf "$APPDIR"
info "已清理 AppDir"


# -----------------------------------------------------------------------------
# 十三、完成提示
# -----------------------------------------------------------------------------
echo ""
echo "============================================================"
echo -e "${GREEN}  EdgeSim AppImage 打包完成${NC}"
echo "============================================================"
echo ""
echo "产物："
echo "  $OUTPUT_APPIMAGE ($OUTPUT_SIZE)"
echo ""
echo "使用方法："
echo "  1. 双击 .AppImage 文件运行（需桌面环境）"
echo "  2. 或命令行执行："
echo "       chmod +x $OUTPUT_APPIMAGE"
echo "       $OUTPUT_APPIMAGE"
echo ""
echo "分发："
echo "  直接复制 .AppImage 到其他 x86_64 Linux 即可运行"
echo "  无需安装依赖（除 glibc 外）"
echo ""
