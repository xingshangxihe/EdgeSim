#!/usr/bin/env bash
# =============================================================================
# gen_chinese_font.sh —— 生成 LVGL 中文字体文件
# =============================================================================
# 【作用】
#   生成 ui_lvgl/lv_font_chinese_16.c（含 ASCII + GB2312 一级常用汉字）。
#   生成后重新 cmake 配置即可自动检测到该文件并定义 HAS_CHINESE_FONT 宏，
#   UI 从此支持中文显示（聊天/OCR/ASR/输入框）。
#
# 【前置依赖】
#   1. node + lv_font_conv（LVGL 官方字体转换工具，npm 全局安装）
#      npm install -g lv_font_conv
#   2. 中文字体（默认用 Ubuntu 的 Noto Sans CJK）：
#      sudo apt install fonts-noto-cjk
#   3. python3（生成 GB2312 常用字码点清单）
#
# 【用法】
#   bash scripts/gen_chinese_font.sh
#   可选：bash scripts/gen_chinese_font.sh /path/to/任意中文字体.ttf
#
# 【输出】
#   ui_lvgl/lv_font_chinese_16.c  —— 约 500KB，勿提交到 git（体积大）
# =============================================================================
set -e   # 任一命令失败立即退出，避免生成残缺文件

# -----------------------------------------------------------------------------
# 一、检查依赖
# -----------------------------------------------------------------------------
if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "[gen_font] 未找到 lv_font_conv。请任选一种方式安装后重跑本脚本："
    echo "  方式 1（推荐）：sudo npm install -g lv_font_conv"
    echo "  方式 2（免全局，改用 npx 调用，无需安装）："
    echo "    将下方 lv_font_conv 命令前的 'lv_font_conv' 换成 'npx --yes lv_font_conv'"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "[gen_font] 错误：需要 python3（生成汉字码点清单）"
    exit 1
fi

# -----------------------------------------------------------------------------
# 二、确定中文字体路径（第一个参数可覆盖）
# -----------------------------------------------------------------------------
# 【修复说明（阶段 4.5 排查实测）】
#   lv_font_conv 1.5.3 无法加载 .ttc 集合格式（Noto CJK 默认就是 .ttc），
#   报错：Cannot load font "...ttc": Unsupported OpenType signature ttcf。
#   因此自动检测时优先选择 .otf/.ttf 格式的中文字体；
#   若用户显式传入 .ttc，给出明确解决指引而不是晦涩报错。
FONT="${1:-}"
if [ -z "$FONT" ]; then
    # 1) 优先：用户曾拆分的 Noto Sans CJK ttf（含 ASCII + 中文，最可靠）
    if [ -f /tmp/NotoSansCJK-2.ttf ]; then
        FONT="/tmp/NotoSansCJK-2.ttf"
        echo "[gen_font] 使用已拆分的 Noto Sans CJK: $FONT"
    else
        # 2) 自动检测：fc-list 列出支持中文的 ttf/otf，
        #    排除 DroidSansFallback（Android 的 CJK fallback 字体，
        #    实测不含 ASCII 字形，用它生成的字库英文/数字缺失）
        CANDIDATE=$(fc-list :lang=zh file 2>/dev/null \
                    | grep -E '\.(ttf|otf)' \
                    | grep -v -i "DroidSansFallback" \
                    | head -1 | cut -d: -f1)
        if [ -n "$CANDIDATE" ]; then
            FONT="$CANDIDATE"
            echo "[gen_font] 自动检测到中文字体: $FONT"
        else
            # 没有合适的 ttf/otf 中文，fallback 到 Noto（.ttc）
            FONT="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
        fi
    fi
fi
if [ ! -f "$FONT" ]; then
    echo "[gen_font] 未找到中文字体: $FONT"
    echo "[gen_font] 请安装：sudo apt install fonts-noto-cjk"
    echo "[gen_font] 或用参数指定字体：bash scripts/gen_chinese_font.sh /path/to/font.ttf"
    exit 1
fi
# 检查 .ttc：lv_font_conv 1.5.3 不支持，给出两条解决路径
case "$FONT" in
    *.ttc)
        echo "[gen_font] 错误：$FONT 是 TTC 集合格式，lv_font_conv 1.5.3 不支持。"
        echo "[gen_font] 解决方式（任选其一）："
        echo "  1) 安装 ttf 中文字体后重跑：sudo apt install -y fonts-droid-fallback"
        echo "     （自动检测会找到 /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf）"
        echo "  2) 拆分 ttc 为 ttf："
        echo "     sudo apt install -y python3-fonttools"
        echo "     python3 -c \"from fontTools.ttLib import TTCollection; \\"
        echo "         [f.save('/tmp/NotoSansCJK-%d.ttf' % i) for i, f in \\"
        echo "          enumerate(TTCollection('$FONT').fonts)]\""
        echo "     bash scripts/gen_chinese_font.sh /tmp/NotoSansCJK-2.ttf"
        exit 1
        ;;
esac
echo "[gen_font] 使用字体: $FONT"

# -----------------------------------------------------------------------------
# 三、生成字符码点清单（ASCII 95 + GB2312 一级 3755 = 3850 个）
# -----------------------------------------------------------------------------
# 【阶段 4.5 三次修复：ASCII 与汉字统一为"单码点"，且控制字库总量】
#   - ASCII 用单码点（0x20,0x21,...,0x7E）而非连续范围 0x20-0x7F：
#     实测发现连续范围与大量单码点混用时，ASCII 字形会被 lv_font_conv
#     丢失（[FONT] 自检 'a'=0），全部统一单码点后不再丢失。
#   - 字库必须 < 1MB：LVGL 8.2 的 glyph_dsc.bitmap_index 是 20 位位域
#     （上限 2^20-1=1048575），全量 CJK（2.1 万字）会溢出并导致字形
#     索引截断、渲染异常（编译 warning: -Woverflow）。3850 字 × 16px
#     bpp4 ≈ 500KB，安全在限内。
# GB2312 一级字：区位码 16-55 区、每区 94 位（55 区到 89 位止），共 3755 字。
# 用 python3 把区位码编码为字节 → gb2312 解码得到汉字 → 输出 Unicode 码点。
# 【阶段 4.5 四次修复】输出"UTF-8 字符集字符串"（非码点清单）：
#   供第四部分用 --symbols 一次性传入，规避 --range 参数过多丢字符的问题。
CHARS_FILE="/tmp/edge_chars.txt"
python3 - "$CHARS_FILE" <<'PY_EOF'
import sys

out_path = sys.argv[1]
chars = []

# 1. ASCII 可见字符（0x20-0x7E，95 个）：英文/数字/符号
for cp in range(0x20, 0x7F):
    chars.append(chr(cp))

# 2. GB2312 一级字（3755 个）
for area in range(16, 56):                 # 区：16~55
    for pos in range(1, 95):               # 位：01~94
        if area == 55 and pos > 89:        # 55 区只到 89 位
            break
        try:
            # GB2312 编码 = (区+0xA0, 位+0xA0)，解码为汉字
            c = bytes([area + 0xA0, pos + 0xA0]).decode('gb2312')
            chars.append(c)
        except Exception:
            pass                            # 空位跳过

# 输出为连续 UTF-8 字符串（无换行），--symbols 需要的就是这种
with open(out_path, 'w', encoding='utf-8') as f:
    f.write(''.join(chars))
print('[gen_font] 字符总数（ASCII 95 + GB2312 一级字）:', len(chars))
PY_EOF

# -----------------------------------------------------------------------------
# 四、调用 lv_font_conv 生成 LVGL 字体文件
# -----------------------------------------------------------------------------
# 参数说明：
#   --no-compress ：不压缩（LVGL 8 自定义格式，生成文件更直观）
#   --no-prefilter：关闭亚像素预滤波（保证清晰度）
#   --bpp 4       ：每像素 4 bit 灰度（质量与体积的平衡）
#   --size 16     ：16px（与 UI 现有 montserrat_14 接近）
#   --range 0x20  --range 0x21 ... ：ASCII + GB2312 汉字全部单码点
#   --format lvgl --output  ：输出 LVGL 源文件
# 【修复说明（阶段 4.5 四次排查）】
#   1. lv_font_conv 的 --range 参数只接受"范围表达式"或"单个码点"，
#      不支持传文件路径。
#   2. --range 参数数量过多时会丢字符（实测：ASCII 与 3755 汉字混用
#      时，无论顺序如何总有其一缺失：'a'=0 或 汉字=0）→ 弃用 --range，
#      改用 --symbols 传 UTF-8 字符集字符串，无数量限制，最可靠。
#   3. 字库必须 < 1MB（LVGL 8.2 glyph_dsc.bitmap_index 是 20 位位域，
#      上限 1048575）：3850 字符 ≈ 500KB，安全；全量 CJK 会溢出。
echo "[gen_font] 正在生成 lv_font_chinese_16.c（约需 1~2 分钟）..."
# --symbols "$CHARS_FILE 内容"：一次传入全部字符（ASCII+GB2312 一级）。
# --lv-include lvgl.h ：控制生成文件顶部 #include 的内容。
#   lv_font_conv 默认生成 #include "lvgl/lvgl.h"（带 lvgl/ 前缀），
#   但本项目 include 路径是 LVGL_PATH/lvgl.h 形式（见根 CMakeLists），
#   会导致编译报 "lvgl/lvgl.h: 没有那个文件或目录"。
#   指定为 lvgl.h 后生成 #include "lvgl.h"，与现有源文件一致。
SYMBOLS=$(cat "$CHARS_FILE")
lv_font_conv \
    --no-compress --no-prefilter \
    --bpp 4 --size 16 \
    --font "$FONT" \
    --symbols "$SYMBOLS" \
    --format lvgl \
    --lv-include lvgl.h \
    --output ui_lvgl/lv_font_chinese_16.c

# -----------------------------------------------------------------------------
# 五、完成提示
# -----------------------------------------------------------------------------
echo "[gen_font] 生成完成: ui_lvgl/lv_font_chinese_16.c"
echo "[gen_font] 下一步：重新配置并编译"
echo "    cd ~/EdgeSim && rm -rf build"
echo "    cmake -S . -B build -DLVGL_PATH=\$HOME/lvgl ... （其余参数照旧）"
echo "    cmake --build build -j\$(nproc)"
echo "[gen_font] 配置时若看到 'UI 模块：检测到中文字体' 即启用成功"
