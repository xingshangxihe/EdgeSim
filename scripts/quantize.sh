#!/bin/bash
# =============================================================================
# EdgeSim 模型量化包装脚本  scripts/quantize.sh
# =============================================================================
# 【脚本作用】
#   把 Qwen1.8B 原始模型（HuggingFace 格式）量化为 INT8 GGUF 格式，
#   供 EdgeSim 的 llm_engine 在 PC 或嵌入式设备上离线推理使用。
#
# 【量化流程】
#   1. 从 HuggingFace 下载 Qwen1.8B 原始权重（pytorch_model.bin / config.json）
#   2. 用 llama.cpp 的 convert.py 转为 GGUF（F16）
#   3. 用 llama.cpp 的 quantize 二进制把 F16 → INT8 (Q8_0)
#   4. 输出 qwen1.8b-int8.gguf，体积约 1.8GB → 1.0GB
#
# 【使用方法】
#   bash scripts/quantize.sh <原始模型目录> <输出gguf路径> [量化类型]
#
#   示例：
#     bash scripts/quantize.sh ./models/qwen1.8b-original ./models/qwen1.8b-int8.gguf q8_0
#
# 【量化类型对照表】
#   q8_0    : 8 位整型（推荐，精度损失最小，体积约 50% 原始）
#   q4_0    : 4 位整型（体积最小，精度损失明显）
#   q4_1    : 4 位带偏移（精度略好于 q4_0）
#   q5_0    : 5 位（折中）
#   f16     : 半精度浮点（不量化，仅转格式）
#
# 【设计文档对应】
#   EdgeSim_Design.md 第 4.3 节「scripts/quantize.sh：调用Python脚本量化Qwen1.8B为INT8」
#   EdgeSim_Design.md 第 1.4.1 节「仅模型量化使用少量Python辅助脚本」
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
# 三、参数解析
# -----------------------------------------------------------------------------
# $1, $2, $3 是脚本位置参数
# ${VAR:-default} 语法：VAR 未设置或为空时用 default

# 原始模型目录：必须包含 config.json、pytorch_model.bin 或 model.safetensors
INPUT_DIR="${1:-}"
# 输出 GGUF 路径
OUTPUT_GGUF="${2:-}"
# 量化类型，默认 q8_0（INT8）
QUANT_TYPE="${3:-q8_0}"

# 校验参数
if [ -z "$INPUT_DIR" ] || [ -z "$OUTPUT_GGUF" ]; then
    echo "用法: $0 <原始模型目录> <输出gguf路径> [量化类型]"
    echo ""
    echo "参数说明："
    echo "  原始模型目录 : 包含 config.json 与 pytorch_model.bin 的目录"
    echo "  输出gguf路径 : 量化后 GGUF 文件路径（如 ./models/qwen-int8.gguf）"
    echo "  量化类型     : q8_0(默认) / q4_0 / q4_1 / q5_0 / f16"
    echo ""
    echo "示例："
    echo "  $0 ./models/qwen1.8b ./models/qwen-int8.gguf q8_0"
    exit 1
fi

# 把输入目录转为绝对路径（脚本可能在任意目录运行）
# realpath 解析符号链接与 ../ 等
INPUT_DIR=$(realpath "$INPUT_DIR")
OUTPUT_GGUF=$(realpath -m "$OUTPUT_GGUF")

info "原始模型目录: $INPUT_DIR"
info "输出 GGUF   : $OUTPUT_GGUF"
info "量化类型    : $QUANT_TYPE"


# -----------------------------------------------------------------------------
# 四、检测原始模型目录结构
# -----------------------------------------------------------------------------
step "1. 校验原始模型目录"

# 必须存在的文件清单
# config.json           : 模型配置（层数、维度、词表大小等）
# tokenizer.json        : 分词器配置
# pytorch_model.bin OR model.safetensors : 模型权重
if [ ! -f "$INPUT_DIR/config.json" ]; then
    error "缺少 config.json，请确认是 HuggingFace 格式模型目录"
fi
if [ ! -f "$INPUT_DIR/tokenizer.json" ]; then
    warn "缺少 tokenizer.json，分词器转换可能失败"
fi

# 检测权重文件存在性（两种格式之一即可）
WEIGHT_FILE=""
if [ -f "$INPUT_DIR/pytorch_model.bin" ]; then
    WEIGHT_FILE="$INPUT_DIR/pytorch_model.bin"
    info "检测到权重文件: pytorch_model.bin"
elif [ -f "$INPUT_DIR/model.safetensors" ]; then
    WEIGHT_FILE="$INPUT_DIR/model.safetensors"
    info "检测到权重文件: model.safetensors"
else
    error "未找到 pytorch_model.bin 或 model.safetensors"
fi

# 打印模型体积（du -sh 显示人类可读大小）
MODEL_SIZE=$(du -sh "$WEIGHT_FILE" | cut -f1)
info "原始模型体积: $MODEL_SIZE"


# -----------------------------------------------------------------------------
# 五、检测 llama.cpp 是否已编译
# -----------------------------------------------------------------------------
step "2. 检测 llama.cpp 工具链"

# llama.cpp 路径：可通过环境变量 LLAMA_PATH 覆盖
# 默认假设 llama.cpp 与 EdgeSim 同级目录
LLAMA_PATH="${LLAMA_PATH:-../llama.cpp}"

# 必需的两个工具：
# 1. convert.py     : Python 脚本，把 HF 格式转 GGUF（F16）
# 2. quantize       : C 二进制，把 F16 GGUF 量化为 INT8
CONVERT_PY="$LLAMA_PATH/convert.py"
QUANTIZE_BIN="$LLAMA_PATH/build/quantize"

if [ ! -f "$CONVERT_PY" ]; then
    error "未找到 convert.py: $CONVERT_PY
请确认 LLAMA_PATH 是否正确：
  LLAMA_PATH=/path/to/llama.cpp $0 $*"
fi

if [ ! -x "$QUANTIZE_BIN" ]; then
    error "未找到 quantize 二进制: $QUANTIZE_BIN
请先用 CMake 编译 llama.cpp：
  cd $LLAMA_PATH && cmake -B build -DLLAMA_BUILD_EXAMPLES=ON && cmake --build build -j"
fi

info "convert.py   : $CONVERT_PY"
info "quantize     : $QUANTIZE_BIN"


# -----------------------------------------------------------------------------
# 六、检测 Python 环境
# -----------------------------------------------------------------------------
step "3. 检测 Python 环境"

# python3 优先，python 兼容
PYTHON_BIN=""
if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN=python
else
    error "未找到 python3 或 python，请先安装 Python 3.8+"
fi
info "Python: $($PYTHON_BIN --version)"

# 检测必需的 Python 包
# llama.cpp 的 convert.py 依赖：
#   torch       : PyTorch，加载 .bin/.safetensors
#   transformers: HuggingFace 模型加载
#   sentencepiece: 分词器
# numpy       : 数组运算
info "检查 Python 依赖包..."
$PYTHON_BIN -c "import torch, transformers, sentencepiece, numpy" 2>/dev/null || {
    warn "缺少 Python 依赖，尝试自动安装..."
    pip3 install torch transformers sentencepiece numpy gguf
}


# -----------------------------------------------------------------------------
# 七、第 1 阶段：HF → GGUF (F16)
# -----------------------------------------------------------------------------
step "4. 转换为 GGUF 格式（F16 中间格式）"

# 临时输出 F16 GGUF 文件
F16_GGUF="${OUTPUT_GGUF%.gguf}.f16.gguf"

info "执行 convert.py: $INPUT_DIR → $F16_GGUF"

# llama.cpp 的 convert.py 参数说明：
#   --outfile      : 输出 GGUF 文件路径
#   --outtype f16  : 输出类型（f16=半精度浮点，未量化）
#   最后一个参数   : 输入模型目录
# 注意：convert.py 内部会调用 transformers 加载模型，需要较大内存（~8GB）
$PYTHON_BIN "$CONVERT_PY" \
    --outfile "$F16_GGUF" \
    --outtype f16 \
    "$INPUT_DIR"

# 校验 F16 GGUF 生成
if [ ! -f "$F16_GGUF" ]; then
    error "F16 GGUF 生成失败"
fi

F16_SIZE=$(du -sh "$F16_GGUF" | cut -f1)
info "F16 GGUF 生成成功，体积: $F16_SIZE"


# -----------------------------------------------------------------------------
# 八、第 2 阶段：F16 → INT8 量化
# -----------------------------------------------------------------------------
step "5. 量化为 $QUANT_TYPE 格式"

info "执行 quantize: $F16_GGUF → $OUTPUT_GGUF ($QUANT_TYPE)"

# llama.cpp 的 quantize 二进制参数：
#   <input>           : 输入 F16 GGUF
#   <output>          : 输出量化 GGUF
#   <quant_type>      : 量化类型（q8_0 / q4_0 / q4_1 / q5_0 / f16）
"$QUANTIZE_BIN" "$F16_GGUF" "$OUTPUT_GGUF" "$QUANT_TYPE"

# 校验量化结果
if [ ! -f "$OUTPUT_GGUF" ]; then
    error "量化失败：$OUTPUT_GGUF 未生成"
fi

INT8_SIZE=$(du -sh "$OUTPUT_GGUF" | cut -f1)
info "量化成功，体积: $INT8_SIZE"


# -----------------------------------------------------------------------------
# 九、清理中间文件
# -----------------------------------------------------------------------------
step "6. 清理中间文件"

# 删除 F16 中间格式（可选，保留以便调试可注释掉）
rm -f "$F16_GGUF"
info "已删除中间文件: $F16_GGUF"


# -----------------------------------------------------------------------------
# 十、输出量化报告
# -----------------------------------------------------------------------------
echo ""
echo "============================================================"
echo -e "${GREEN}  EdgeSim 模型量化完成${NC}"
echo "============================================================"
echo ""
echo "量化报告："
echo "  原始模型 : $INPUT_DIR"
echo "  原始体积 : $MODEL_SIZE"
echo "  量化类型 : $QUANT_TYPE"
echo "  量化体积 : $INT8_SIZE"
echo "  压缩比   : $(echo "scale=2; $(stat -c%s "$OUTPUT_GGUF") * 100 / $(stat -c%s "$WEIGHT_FILE")" | bc)%"
echo "  输出文件 : $OUTPUT_GGUF"
echo ""
echo "下一步："
echo "  1. 把 $OUTPUT_GGUF 复制到 EdgeSim 的模型目录"
echo "  2. 启动 EdgeSim，llm_engine 会自动加载此文件"
echo "  3. 真实编译模式（CMake，阶段 0 起唯一构建方式）："
echo "       cd EdgeSim && mkdir build && cd build"
echo "       cmake .. -DLLAMA_PATH=$LLAMA_PATH -DWITH_LLAMA=ON"
echo "       make"
echo ""
