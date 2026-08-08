#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
EdgeSim 模型量化 Python 实现  scripts/quantize.py
=============================================================================
【文件作用】
  本脚本是 quantize.sh 的 Python 实现版本，提供更精细的控制：
    1. 从 HuggingFace 加载 Qwen1.8B 原始权重
    2. 转换为 GGUF 格式（F16 中间格式）
    3. 量化为 INT8 (Q8_0) 或其他类型
    4. 输出量化前后体积对比报告

【为什么需要 Python 脚本？】
  设计文档第 1.4.1 节明确允许"模型量化使用少量 Python 辅助脚本"。
  llama.cpp 自带的 convert.py 是 Python 写的，本脚本是对其调用的封装，
  并增加了进度显示、错误处理、报告生成等增值功能。

【使用方法】
  python3 scripts/quantize.py \\
      --input  ./models/qwen1.8b-original \\
      --output ./models/qwen1.8b-int8.gguf \\
      --quant  q8_0

【依赖安装】
  pip3 install torch transformers sentencepiece numpy gguf

【设计文档对应】
  EdgeSim_Design.md 第 4.3 节「scripts/quantize.sh：调用Python脚本量化」
=============================================================================
"""

import os           # 文件路径操作
import sys          # 命令行参数与退出码
import argparse     # 命令行参数解析（比 sys.argv 更友好）
import subprocess   # 调用外部命令（quantize 二进制）
import time         # 计时
from pathlib import Path   # 面向对象的路径操作（比 os.path 更现代）


# -----------------------------------------------------------------------------
# 颜色输出（与 Shell 脚本保持一致的视觉风格）
# -----------------------------------------------------------------------------
# ANSI 转义码：让 Python print 也带颜色
class Color:
    RED    = '\033[31m'
    GREEN  = '\033[32m'
    YELLOW = '\033[33m'
    BLUE   = '\033[34m'
    NC     = '\033[0m'   # No Color

def info(msg):
    """打印普通信息（绿色前缀）"""
    print(f"{Color.GREEN}[INFO]{Color.NC}  {msg}")

def warn(msg):
    """打印警告信息（黄色前缀）"""
    print(f"{Color.YELLOW}[WARN]{Color.NC}  {msg}")

def error(msg):
    """打印错误信息并退出（红色前缀）"""
    print(f"{Color.RED}[ERROR]{Color.NC} {msg}")
    sys.exit(1)

def step(msg):
    """打印步骤标记（蓝色前缀）"""
    print(f"{Color.BLUE}[STEP]{Color.NC}  {msg}")


# -----------------------------------------------------------------------------
# 支持的量化类型表
# -----------------------------------------------------------------------------
# key   : 命令行参数值
# value : (llama.cpp 类型名, 描述, 压缩比近似)
# 压缩比相对于 F16，约值仅供参考
QUANT_TYPES = {
    'q8_0' : ('q8_0', '8位整型（推荐，精度损失最小）', 0.50),
    'q4_0' : ('q4_0', '4位整型（体积最小）',           0.25),
    'q4_1' : ('q4_1', '4位带偏移（精度略好）',         0.30),
    'q5_0' : ('q5_0', '5位折中',                       0.35),
    'f16'  : ('f16',  '半精度浮点（不量化）',           1.00),
}


def parse_args():
    """
    解析命令行参数

    使用 argparse 模块，自动生成 --help 帮助文本。
    比手动解析 sys.argv 更健壮，支持类型检查与默认值。
    """
    # 创建 ArgumentParser 对象
    # description 是 --help 顶部的说明文字
    parser = argparse.ArgumentParser(
        description='EdgeSim Qwen1.8B 模型量化脚本',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例：
  python3 %(prog)s --input ./models/qwen1.8b --output ./models/qwen-int8.gguf
  python3 %(prog)s --input ./models/qwen1.8b --output ./models/qwen-q4.gguf --quant q4_0
        """.strip()
    )

    # 添加参数定义
    # --input : 必填，原始模型目录
    parser.add_argument(
        '--input', '-i',
        required=True,
        help='原始模型目录（包含 config.json 与 pytorch_model.bin）'
    )

    # --output : 必填，输出 GGUF 路径
    parser.add_argument(
        '--output', '-o',
        required=True,
        help='输出 GGUF 文件路径（如 ./models/qwen-int8.gguf）'
    )

    # --quant : 可选，默认 q8_0
    # choices 限制可选值，输入非法值时 argparse 自动报错
    parser.add_argument(
        '--quant', '-q',
        choices=list(QUANT_TYPES.keys()),
        default='q8_0',
        help='量化类型（默认 q8_0，可选 q4_0/q4_1/q5_0/f16）'
    )

    # --llama-path : 可选，llama.cpp 根目录
    parser.add_argument(
        '--llama-path',
        default=os.environ.get('LLAMA_PATH', '../llama.cpp'),
        help='llama.cpp 根目录（默认读环境变量 LLAMA_PATH 或 ../llama.cpp）'
    )

    # --keep-f16 : 可选，保留 F16 中间文件
    # action='store_true' 表示出现该 flag 即为 True
    parser.add_argument(
        '--keep-f16',
        action='store_true',
        help='保留 F16 中间文件（调试用）'
    )

    return parser.parse_args()


def check_python_deps():
    """
    检查 Python 依赖包是否已安装

    通过 import 测试，失败则提示用户安装。
    """
    step('检查 Python 依赖包')

    # 依赖列表：包名 → 用途
    deps = {
        'torch':         'PyTorch，加载模型权重',
        'transformers':  'HuggingFace 模型加载',
        'sentencepiece': '分词器',
        'numpy':         '数组运算',
    }

    missing = []
    for pkg, desc in deps.items():
        try:
            # __import__ 是 import 语句的函数形式
            # 用 try/except 检测是否可导入
            __import__(pkg)
            info(f'  ✓ {pkg} ({desc})')
        except ImportError:
            missing.append(pkg)
            warn(f'  ✗ {pkg} ({desc})')

    if missing:
        # 把缺失的包名用空格连成一行
        pkg_str = ' '.join(missing)
        error(f'缺少依赖包：{missing}\n请安装：pip3 install {pkg_str}')


def check_input_dir(input_dir: Path):
    """
    校验原始模型目录结构

    Args:
        input_dir: 模型目录 Path 对象
    """
    step(f'校验原始模型目录: {input_dir}')

    # 必需文件清单
    required_files = ['config.json']
    # 权重文件二选一
    weight_files = ['pytorch_model.bin', 'model.safetensors']

    # 检查必需文件
    for f in required_files:
        if not (input_dir / f).exists():
            error(f'缺少必需文件: {f}')

    # 检查权重文件
    weight_path = None
    for f in weight_files:
        p = input_dir / f
        if p.exists():
            weight_path = p
            info(f'检测到权重文件: {f}')
            break
    if weight_path is None:
        error(f'未找到权重文件，需要以下之一: {weight_files}')

    # 检查分词器
    tokenizer = input_dir / 'tokenizer.json'
    if not tokenizer.exists():
        warn('缺少 tokenizer.json，分词器转换可能失败')

    # 返回权重文件路径与大小
    # .stat().st_size 是字节数
    size_bytes = weight_path.stat().st_size
    size_mb = size_bytes / (1024 * 1024)
    info(f'原始模型体积: {size_mb:.1f} MB')

    return weight_path, size_mb


def check_llama_tools(llama_path: Path):
    """
    检查 llama.cpp 的 convert.py 与 quantize 二进制

    Args:
        llama_path: llama.cpp 根目录
    Returns:
        (convert_py_path, quantize_bin_path)
    """
    step(f'检查 llama.cpp 工具: {llama_path}')

    convert_py = llama_path / 'convert.py'
    # quantize 在 build/ 子目录（Linux）或 build/Release/（Windows）
    quantize_bin = llama_path / 'build' / 'quantize'
    if not quantize_bin.exists():
        # Windows 路径回退
        quantize_bin = llama_path / 'build' / 'Release' / 'quantize.exe'

    if not convert_py.exists():
        error(f'未找到 convert.py: {convert_py}')

    if not quantize_bin.exists():
        error(f'未找到 quantize 二进制: {quantize_bin}\n'
              f'请先编译 llama.cpp: cd {llama_path} && make')

    info(f'  convert.py : {convert_py}')
    info(f'  quantize   : {quantize_bin}')
    return convert_py, quantize_bin


def run_convert(convert_py: Path, input_dir: Path, output_f16: Path):
    """
    第 1 阶段：HF 格式 → GGUF F16

    调用 llama.cpp 的 convert.py 完成转换。
    使用 subprocess.run 执行子进程，捕获输出。

    Args:
        convert_py:  convert.py 脚本路径
        input_dir:   输入模型目录
        output_f16:  输出 F16 GGUF 路径
    """
    step(f'阶段 1: HF → GGUF F16')

    # 构造命令列表（subprocess 推荐用列表形式，避免 shell 注入）
    cmd = [
        sys.executable,           # 当前 Python 解释器路径
        str(convert_py),
        '--outfile', str(output_f16),
        '--outtype', 'f16',
        str(input_dir),
    ]
    info(f'执行: {" ".join(cmd)}')

    # subprocess.run 执行子进程
    # check=True : 子进程返回非 0 时抛出 CalledProcessError
    # 捕获 stdout/stderr 自动打印
    start = time.time()
    result = subprocess.run(cmd, check=False)

    if result.returncode != 0:
        error(f'convert.py 失败，返回码: {result.returncode}')

    elapsed = time.time() - start
    info(f'F16 转换完成，耗时 {elapsed:.1f}s')

    if not output_f16.exists():
        error(f'F16 GGUF 未生成: {output_f16}')

    size_mb = output_f16.stat().st_size / (1024 * 1024)
    info(f'F16 GGUF 体积: {size_mb:.1f} MB')


def run_quantize(quantize_bin: Path, f16_path: Path, output_path: Path, quant_type: str):
    """
    第 2 阶段：F16 → INT8 量化

    调用 llama.cpp 的 quantize 二进制完成量化。

    Args:
        quantize_bin: quantize 二进制路径
        f16_path:     输入 F16 GGUF
        output_path:  输出量化 GGUF
        quant_type:   量化类型（如 q8_0）
    """
    step(f'阶段 2: F16 → {quant_type}')

    cmd = [
        str(quantize_bin),
        str(f16_path),
        str(output_path),
        quant_type,
    ]
    info(f'执行: {" ".join(cmd)}')

    start = time.time()
    result = subprocess.run(cmd, check=False)

    if result.returncode != 0:
        error(f'quantize 失败，返回码: {result.returncode}')

    elapsed = time.time() - start
    info(f'量化完成，耗时 {elapsed:.1f}s')

    if not output_path.exists():
        error(f'量化 GGUF 未生成: {output_path}')

    size_mb = output_path.stat().st_size / (1024 * 1024)
    info(f'量化后体积: {size_mb:.1f} MB')


def print_report(input_size_mb: float, output_path: Path, quant_type: str):
    """
    打印量化报告

    Args:
        input_size_mb: 原始模型体积（MB）
        output_path:   量化后文件路径
        quant_type:    量化类型
    """
    output_size_mb = output_path.stat().st_size / (1024 * 1024)
    ratio = output_size_mb / input_size_mb * 100 if input_size_mb > 0 else 0

    print()
    print('=' * 60)
    print(f'{Color.GREEN}  EdgeSim 模型量化报告{Color.NC}')
    print('=' * 60)
    print()
    print(f'  原始体积 : {input_size_mb:.1f} MB')
    print(f'  量化类型 : {quant_type} ({QUANT_TYPES[quant_type][1]})')
    print(f'  量化体积 : {output_size_mb:.1f} MB')
    print(f'  压缩比   : {ratio:.1f}%')
    print(f'  输出文件 : {output_path}')
    print()
    print('下一步：')
    print(f'  1. 把 {output_path} 复制到 EdgeSim 模型目录')
    print('  2. 启动 EdgeSim，llm_engine 会自动加载此文件')
    print()


def main():
    """
    主函数：解析参数 → 校验环境 → 转换 → 量化 → 报告
    """
    # 1. 解析命令行参数
    args = parse_args()

    # 2. 转换为 Path 对象（面向对象路径操作）
    input_dir = Path(args.input).resolve()
    output_path = Path(args.output).resolve()
    llama_path = Path(args.llama_path).resolve()

    # 3. 校验量化类型
    if args.quant not in QUANT_TYPES:
        error(f'未知量化类型: {args.quant}')

    # 4. 创建输出目录（如 ./models/ 不存在）
    # parents=True : 父目录也一并创建（类似 mkdir -p）
    # exist_ok=True : 目录已存在不报错
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # 5. F16 中间文件路径（在输出文件名加 .f16 后缀）
    # .stem 是不带后缀的文件名，.suffix 是后缀
    f16_path = output_path.with_suffix('.f16.gguf')

    # 6. 依次执行各阶段
    check_python_deps()
    weight_path, input_size_mb = check_input_dir(input_dir)
    convert_py, quantize_bin = check_llama_tools(llama_path)
    run_convert(convert_py, input_dir, f16_path)
    run_quantize(quantize_bin, f16_path, output_path, QUANT_TYPES[args.quant][0])

    # 7. 清理中间文件
    if not args.keep_f16:
        step('清理 F16 中间文件')
        f16_path.unlink(missing_ok=True)   # missing_ok=True 文件不存在不报错
        info(f'已删除: {f16_path}')
    else:
        info(f'保留 F16 中间文件: {f16_path}')

    # 8. 打印报告
    print_report(input_size_mb, output_path, args.quant)


# -----------------------------------------------------------------------------
# 脚本入口
# -----------------------------------------------------------------------------
# __name__ == '__main__' 表示脚本被直接运行（而非被 import）
# 这是 Python 脚本的标准入口写法
if __name__ == '__main__':
    main()
