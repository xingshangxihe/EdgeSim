# EdgeSim

> 离线 AI 客户端 + 嵌入式硬件仿真器：纯 C/C++、LVGL 原生界面、CMake 构建

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Language: C/C++](https://img.shields.io/badge/Language-C%2FC%2B%2B-blue.svg)](https://github.com/topics/c)
[![Build: CMake](https://img.shields.io/badge/Build-CMake-green.svg)](https://cmake.org/)
[![GUI: LVGL](https://img.shields.io/badge/GUI-LVGL-orange.svg)](https://lvgl.io/)

EdgeSim 是一个面向**普通用户**与**嵌入式开发者**的双用途离线 AI 工具：

- **普通用户**：在低配电脑上跑离线 AI，支持对话、OCR、语音转写，全程断网，零隐私泄露。
- **嵌入式开发者**：内置硬件内存仿真器，在 PC 上模拟 T153 / RV1106 等低成本 Linux 开发板的小内存环境；三层解耦代码可直接移植到嵌入式硬件。

市面稀缺的「离线 AI 客户端 + 嵌入式硬件仿真」二合一开源工程。

---

## 目录

- [核心特色](#核心特色)
- [功能状态](#功能状态)
- [架构设计](#架构设计)
- [快速开始](#快速开始)
- [编译构建](#编译构建)
- [模型准备](#模型准备)
- [运行说明](#运行说明)
- [嵌入式硬件仿真](#嵌入式硬件仿真-核心特色)
- [文档导航](#文档导航)
- [常见问题](#常见问题-faq)
- [开源协议](#开源协议)

---

## 核心特色

### 1. 三大离线 AI 引擎

| 引擎 | 模型 | 功能 | 后端 |
|------|------|------|------|
| LLM 对话 | Qwen2.5-1.5B (Q4_K_M) | 自然语言对话、文案创作、翻译 | llama.cpp |
| OCR 识别 | PP-OCRv3 (ONNX) | 图片文字提取，支持中英文混排 | ONNX Runtime |
| ASR 语音 | Whisper Base (Q8) | 麦克风录音转文字 | whisper.cpp |

三大引擎均在本地 CPU 上推理，**全程零网络请求**。

### 2. 三层解耦架构

```
表现层 (ui_lvgl)  →  业务层 (business)  →  AI 引擎层 (ai_engine)
```

- 表现层不直接调用 AI 引擎层，所有跨层通信走管道（multi_proc）
- 业务层与 AI 引擎层完全独立，可单独复用
- **仅修改外设驱动代码即可移植到 T153 / RV1106 开发板**

### 3. 纯 C/C++ + LVGL 原生界面

- 核心业务全部 C/C++，无 Python 核心逻辑
- LVGL v8.2 + SDL2 桌面模拟器，无 Qt / WebUI / HTML 前端
- CMake 构建，支持 x86 桌面与 ARM64 交叉编译

---

## 功能状态

### 普通用户功能

| 功能 | 状态 | 说明 |
|------|------|------|
| 离线大模型对话 | ✅ 已实现 | Qwen2.5-1.5B，支持中文对话、上下文续接 |
| Whisper 离线语音 | ✅ 已实现 | 录音 → ASR 转写 → LLM 回复，全链路打通 |
| OCR 图片识别 | ✅ 已实现 | 导入图片 → PP-OCR 识别 → 结果存数据库 |
| LVGL 图形界面 | ✅ 已实现 | 聊天窗口 / 文件导入 / 悬浮窗 / 中文显示 |
| 本地 RAG 知识库 | 🚧 开发中 | 文件导入面板已有，检索逻辑待实现 |

### 嵌入式开发者功能

| 功能 | 状态 | 说明 |
|------|------|------|
| 4 进程隔离架构 | ✅ 已实现 | UI + LLM + OCR + ASR，管道 IPC 通信 |
| SQLite 持久化 | ✅ 已实现 | 对话历史 / 知识库索引 / 性能日志三表 |
| 本地 RAG 知识库 | ✅ 已实现 | 中文检索 + 答案来源标注 |
| 中文支持 | ✅ 已实现 | 中文输入 / 显示 / 检索 / LLM 中文回答 |
| 硬件内存仿真 | ✅ 已实现 | 真实模型体积 + 动态曲线 + OOM 保护演示 |
| 性能报表 | ✅ 已实现 | perf_log 入库 + perf_report.txt 报表 |
| ARM64 交叉编译 | 🚧 脚本就绪 | 一键脚本 build_arm64.sh，待真机验证 |

---

## 架构设计

### 三层解耦（严格遵守，禁止跨层调用）

```
┌──────────────────────────────────────────────────────────┐
│  表现层 ui_lvgl（LVGL 界面）                              │
│  - 主对话窗口、文件导入、悬浮窗                           │
│  - 仅界面逻辑，不处理 AI 推理 / 内存管理                  │
└────────────────────────┬─────────────────────────────────┘
                         │ 管道（TaskData）
┌────────────────────────▼─────────────────────────────────┐
│  业务层 business（核心控制中心）                          │
│  ├── hardware_sim  内存仿真 / 上限管控 / OOM 保护         │
│  ├── multi_proc    4 进程 fork / 管道 IPC / 信号处理     │
│  └── sqlite_db     对话 / 知识库 / 日志持久化             │
└────────────────────────┬─────────────────────────────────┘
                         │ 直接调用
┌────────────────────────▼─────────────────────────────────┐
│  AI 引擎层 ai_engine（仅封装第三方库）                    │
│  ├── llm_engine    llama.cpp 封装（统一 init/run/destroy）│
│  ├── ocr_engine    ONNX Runtime + PP-OCR 封装             │
│  └── asr_engine    whisper.cpp 封装                       │
└──────────────────────────────────────────────────────────┘
```

### 4 进程隔离架构

```
                      ┌─── pipe_to_llm ──→ [LLM 子进程]
[UI 主进程(父)] ─────┼─── pipe_to_ocr ──→ [OCR 子进程]
                      └─── pipe_to_asr ──→ [ASR 子进程]
```

每条管道含 2 根单向 pipe（父→子 + 子→父），实现双向通信。子进程崩溃由父进程自动重启。

### 目录结构

```
EdgeSim/
├── CMakeLists.txt           # 根目录 CMake 构建入口（唯一构建方式）
├── main.c                   # 主程序入口（4 进程编排 + UI 启动）
├── EdgeSim_Design.md        # 完整需求与架构设计文档
├── README.md                # 本文档
│
├── ui_lvgl/                 # 表现层（LVGL 界面）
│   ├── CMakeLists.txt       #   本模块 CMake 构建脚本
│   ├── ui_lvgl.h            #   表现层对外接口声明
│   ├── ui_pipeline.h        #   UI 管道通信适配接口
│   ├── ui_main.c            #   SDL2 + LVGL 初始化、键盘/鼠标驱动
│   ├── ui_chat_window.c     #   聊天窗口（消息气泡、输入框）
│   ├── ui_floating_window.c #   悬浮快捷窗（Hide/Rec/OCR/Exit 按钮）
│   ├── ui_file_import.c     #   文件导入面板（路径输入+类型选择）
│   ├── ui_mem_monitor.c     #   内存监控面板（待接入）
│   ├── ui_pipeline.c        #   UI 与子进程的管道通信适配
│   └── test_ui_lvgl.c       #   本模块单元测试
│
├── business/                # 业务层
│   ├── hardware_sim/        #   硬件内存仿真器
│   │   ├── CMakeLists.txt
│   │   ├── hardware_sim.h   #     内存仿真接口声明
│   │   ├── hardware_sim.c   #     内存仿真实现
│   │   └── test_hardware_sim.c
│   ├── multi_proc/          #   多进程 fork + 管道 IPC
│   │   ├── CMakeLists.txt
│   │   ├── multi_proc.h
│   │   ├── multi_proc.c
│   │   └── test_multi_proc.c
│   └── sqlite_db/           #   SQLite 数据库封装
│       ├── CMakeLists.txt
│       ├── sqlite_db.h
│       ├── sqlite_db.c
│       └── test_sqlite_db.c
│
├── ai_engine/               # AI 引擎层
│   ├── llm_engine/          #   llama.cpp 封装
│   │   ├── CMakeLists.txt
│   │   ├── llm_engine.h
│   │   ├── llm_engine.c
│   │   └── test_llm_engine.c
│   ├── ocr_engine/          #   ONNX Runtime + PP-OCR 封装
│   │   ├── CMakeLists.txt
│   │   ├── ocr_engine.h
│   │   ├── ocr_engine.c     #     编译为 C++（用 ONNX Runtime + OpenCV）
│   │   └── test_ocr_engine.c
│   └── asr_engine/          #   whisper.cpp 封装
│       ├── CMakeLists.txt
│       ├── asr_engine.h
│       ├── asr_engine.c
│       └── test_asr_engine.c
│
├── cross_compile/           # 交叉编译配置
│   ├── toolchain-arm64.cmake #   ARM64 CMake 工具链文件（唯一入口）
│   ├── env_arm64.sh          #   ARM64 交叉编译环境一键安装脚本
│   ├── build_arm64.sh        #   ARM64 交叉编译一键脚本（阶段 5）
│   └── termux_build.sh       #   Termux 一键部署脚本
│
├── scripts/                 # 辅助脚本
│   ├── quantize.sh          #   LLM 模型量化 Shell 脚本
│   ├── quantize.py          #   LLM 模型量化 Python 脚本
│   ├── build_appimage.sh    #   AppImage 打包脚本
│   └── termux_deploy.sh     #   Termux 部署脚本
│
└── models/                  # AI 模型文件目录（运行前需准备，见下文）
    ├── llm/                 #   Qwen2.5-1.5B GGUF
    ├── ocr/                 #   PP-OCR ONNX (det/cls/rec) + 字典
    └── asr/                 #   Whisper Base GGML
```

> **说明**：项目统一使用 CMake 构建，第三方库通过 CMake `find_package` 自动检测或路径变量显式指定。

详细设计见 [EdgeSim_Design.md](EdgeSim_Design.md)。

---

## 快速开始

### 环境要求

- **操作系统**：Ubuntu 20.04+ / Debian 11+ / WSL2
- **编译器**：gcc 9+ / g++ 9+
- **CMake**：3.16+（Ubuntu 20.04 自带 3.16，无需额外安装）
- **依赖库**：SDL2、SQLite3、OpenCV 4.x（OCR 预处理）
- **磁盘空间**：3GB（含模型文件）

### 一键安装依赖（Ubuntu）

```bash
sudo apt update
sudo apt install -y build-essential cmake libsdl2-dev libsqlite3-dev \
                    libopencv-dev wget git
```

### 编译运行

```bash
git clone https://github.com/<your>/EdgeSim.git
cd EdgeSim

# 1. 创建构建目录
mkdir build && cd build

# 2. CMake 配置（指定第三方库路径）
cmake .. \
    -DLVGL_PATH=/home/<user>/lvgl \
    -DLLAMA_PATH=/home/<user>/llama.cpp \
    -DWHISPER_PATH=/home/<user>/whisper.cpp \
    -DWITH_LLAMA=ON \
    -DWITH_WHISPER=ON

# 3. 编译
make

# 4. 准备模型文件（见下文）
ln -s ~/EdgeSim/models models

# 5. 运行
./bin/edgesim
```

> **首次体验（零依赖 mock 模式）**：若只想先编译模块测试程序、暂不准备 LVGL/模型，
> 可用 `cmake .. -DBUILD_EDGESIM=OFF`——此时未设置 `LVGL_PATH` 会自动跳过
> ui_lvgl 子模块编译，不影响业务层/AI 引擎层测试程序的构建与运行（见下文）。

---

## 编译构建

### CMake 选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `WITH_LLAMA` | 启用 llama.cpp（LLM 推理） | OFF |
| `WITH_WHISPER` | 启用 whisper.cpp（ASR 语音） | OFF |
| `WITH_RKNN` | 启用 RKNN Lite（OCR，Rockchip NPU） | OFF |
| `WITH_OPENCV` | 启用 OpenCV（OCR 图像预处理） | AUTO |
| `WITH_ONNXRUNTIME` | 启用 ONNX Runtime（OCR 推理后端） | AUTO |
| `WITH_RKNPU` | 预留：瑞芯微 RKNPU 后端（暂未实际链接） | OFF |
| `WITH_TENSORRT` | 预留：NVIDIA TensorRT 后端（暂未实际链接） | OFF |
| `BUILD_TESTS` | 编译各模块测试程序（test_*） | ON |
| `BUILD_EDGESIM` | 链接最终 edgesim 可执行文件 | ON |
| `NO_SDL` | 禁用 SDL2（ARM64 交叉编译时用） | OFF |

> **AUTO 选项**：`WITH_OPENCV` / `WITH_ONNXRUNTIME` 设为 `AUTO` 时由 `find_package`
> 自动检测，找到则启用对应宏（`HAS_OPENCV` / `HAS_ONNXRUNTIME`），找不到则跳过。
> 强制要求时改为 `ON`，未找到将报错。

### 路径变量（CACHE PATH）

第三方库路径通过 CMake 变量指定，持久化到 `build/CMakeCache.txt`，下次 cmake 时自动读取：

| 变量 | 说明 | 是否必需 |
|------|------|----------|
| `LVGL_PATH` | LVGL 根目录（含 `lvgl.h` 与 `build/liblvgl.a`） | 是（启用 UI 时） |
| `LVGL_LIBRARY` | LVGL 静态库路径（交叉编译时显式指定 ARM64 版 `liblvgl.a`，避免链错 x86 库） | 否（默认自动查找 `build-arm64/` → `build/lib/` → `build/`） |
| `LLAMA_PATH` | llama.cpp 根目录（含 `include/` 与 `build/libllama.so`） | 是（`WITH_LLAMA=ON` 时） |
| `WHISPER_PATH` | whisper.cpp 根目录（含 `include/` 与 `build/libwhisper.so`） | 是（`WITH_WHISPER=ON` 时） |
| `ONNXRUNTIME_PATH` | ONNX Runtime 安装目录（含 `include/onnxruntime_c_api.h`） | 是（`WITH_ONNXRUNTIME=ON` 时，AUTO 模式可读环境变量 `ONNXRUNTIME_ROOT`） |
| `RKNN_PATH` | RKNN Lite SDK 根目录（含 `include/rknn_api.h` 与 `lib/librknnrt.so`） | 是（`WITH_RKNN=ON` 时） |
| `STB_PATH` | stb_image 头文件目录（含 `stb_image.h`） | 否（RKNN 模式下用） |

完整配置示例：

```bash
cmake .. \
    -DLVGL_PATH=/home/<user>/lvgl \
    -DLLAMA_PATH=/home/<user>/llama.cpp \
    -DWHISPER_PATH=/home/<user>/whisper.cpp \
    -DONNXRUNTIME_PATH=/home/<user>/onnxruntime \
    -DWITH_LLAMA=ON \
    -DWITH_WHISPER=ON
```

> **注意**：路径必须用绝对路径，不要用 `~`，否则 CMake 解析失败。

### x86 桌面编译

```bash
mkdir build && cd build
cmake .. -DLVGL_PATH=... -DLLAMA_PATH=... -DWITH_LLAMA=ON -DWITH_WHISPER=ON
make
./bin/edgesim
```

### 仅编译测试程序（不链接第三方库 / 无 LVGL）

```bash
mkdir build && cd build
cmake .. -DBUILD_EDGESIM=OFF
make
./bin/test_sqlite_db
./bin/test_hardware_sim
./bin/test_llm_engine
```

> **说明**：`BUILD_EDGESIM=OFF` 时不链接最终可执行文件，三个 AI 引擎默认以
> mock 桩模式编译（不依赖 llama.cpp / whisper.cpp / ONNX Runtime），
> 便于在没有第三方库与模型时先验证各模块接口。
> 若同时未设置 `LVGL_PATH`，ui_lvgl 子模块会被自动跳过（其源码依赖 `lvgl.h`）。

### ARM64 交叉编译（一键脚本）

目标平台：全志 T153 / 瑞芯微 RV1106 / 安卓 Termux（均 aarch64）。

```bash
# 1. 安装交叉编译工具链（一次即可）
sudo bash cross_compile/env_arm64.sh

# 2. 一键交叉编译（自动完成：工具链检查 → ARM64 sqlite3 → LVGL 交叉编译 → EdgeSim）
bash cross_compile/build_arm64.sh

# 3. 验证产物架构（应显示 ARM aarch64）
file build-arm64/bin/edgesim

# 4. 在 PC 上用 QEMU 跑测试（mock 模式，无 UI）
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./build-arm64/bin/test_hardware_sim
```

**脚本选项**：
- `--skip-lvgl`：跳过 LVGL 交叉编译（仅验证工具链与业务层）
- `--lvgl-path <路径>`：指定 LVGL 源码目录（默认 `$HOME/lvgl`）

**关键设计（阶段 5）**：
- **LVGL 库架构隔离**：脚本用 aarch64 工具链把 LVGL 编译到 `LVGL_PATH/build-arm64/`，
  并通过 `-DLVGL_LIBRARY` 显式传给 EdgeSim——避免误链 x86 的 `build/liblvgl.a`
  （会报 `Skipping incompatible file`）。
- **sqlite3 多架构**：脚本自动启用 arm64 架构并安装 `libsqlite3-dev:arm64`。
- **AI 引擎 mock 模式**：交叉编译默认不依赖 llama/whisper/onnx（真实库的 ARM64
  版本按需通过 `-DLLAMA_PATH`/`-DWHISPER_PATH` 等追加）。
- **NO_SDL 自动生效**：工具链文件自动定义 `NO_SDL` 宏，UI 层编译为无桌面桩，
  不依赖 SDL2。

**手动交叉编译**（不依赖一键脚本）：
```bash
mkdir build-arm64 && cd build-arm64
cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake \
      -DLVGL_PATH=$HOME/lvgl -DLVGL_LIBRARY=$HOME/lvgl/build-arm64/lib/liblvgl.a ..
make
```

---

## 中文字体生成（UI 中文显示必需）

UI 中文显示依赖 `ui_lvgl/lv_font_chinese_16.c`（lv_font_conv 生成的字体文件，
约 3MB，已加入 `.gitignore` 不随仓库分发，clone 后需自行生成）：

```bash
# 1. 安装工具（一次即可）
sudo apt install -y fonts-noto-cjk fonts-droid-fallback
sudo npm install -g lv_font_conv@1.3.0   # 注意：必须 1.3.0（LVGL 8 兼容）

# 2. 生成字体（ASCII + GB2312 一级汉字，约 1~2 分钟）
bash scripts/gen_chinese_font.sh

# 3. 验证生成成功
grep -c "glyph_cnt" ui_lvgl/lv_font_chinese_16.c   # 输出 ≥ 1
```

**说明**：
- 字体用 LVGL **fallback 链**：汉字用 `lv_font_chinese_16`，英文/数字自动回退
  内置 `montserrat_14`，无需把 ASCII 编入中文字库。
- 生成后重新 cmake 配置即可（CMake 检测到字体文件自动定义 `HAS_CHINESE_FONT`）：
  ```bash
  cmake -S . -B build -DLVGL_PATH=$HOME/lvgl   # 其他参数照旧
  ```

---

## 模型准备

EdgeSim 需要三个离线模型文件，放在 `models/` 目录：

```bash
mkdir -p models/llm models/ocr models/asr
```

### 1. LLM 模型（Qwen2.5-1.5B）

```bash
# 下载 Q4_K_M 量化版（约 1GB）
# 放到 models/llm/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

文件名固定为 `qwen2.5-1.5b-instruct-q4_k_m.gguf`。

### 2. OCR 模型（PP-OCRv3 ONNX）

```bash
# 需要四个文件，放到 models/ocr/ 目录：
#   det.onnx              — 文本检测模型
#   cls.onnx              — 方向分类模型
#   rec.onnx              — 文字识别模型
#   ppocr_keys_v1.txt     — 识别字典（6623 个字符）
```

### 3. ASR 模型（Whisper Base）

```bash
# 下载 ggml-base-q8_0.bin（约 78MB）
# 放到 models/asr/ggml-base-q8_0.bin
```

文件名固定为 `ggml-base-q8_0.bin`。

---

## 运行说明

### 共享库路径

EdgeSim 依赖多个共享库，运行前需设置 `LD_LIBRARY_PATH`：

```bash
export LD_LIBRARY_PATH=\
/home/<user>/llama.cpp/build/bin:\
/home/<user>/whisper.cpp/build/bin:\
/home/<user>/onnxruntime/lib:\
$LD_LIBRARY_PATH
```

### 模型目录软链接

在 `build/` 目录下创建 `models` 软链接，使程序能通过相对路径找到模型：

```bash
cd build
ln -s ~/EdgeSim/models models
```

### 数据库

程序启动时自动在 `~/EdgeSim/data/edgesim.db` 创建数据库，包含三张表：

| 表名 | 用途 |
|------|------|
| `chat_history` | 对话历史（用户提问 + AI 回答 + OCR 识别结果） |
| `kb_files` | 知识库文件索引（图片/文本路径） |
| `perf_log` | 性能日志（推理延迟、内存峰值） |

查看数据库内容：

```bash
sqlite3 ~/EdgeSim/data/edgesim.db
sqlite> SELECT * FROM chat_history;
sqlite> SELECT * FROM kb_files;
sqlite> .quit
```

---

## 嵌入式硬件仿真 （核心特色）

### 为什么需要内存仿真？

嵌入式开发板（如全志 T153、瑞芯微 RV1106）内存只有 128~512MB，而 PC 开发环境动辄 16GB。直接在 PC 上调试 AI 代码无法发现：

- 内存不足导致 OOM 崩溃
- 多模型同时加载超出限制
- 长时间运行内存泄漏累积

EdgeSim 的 `business/hardware_sim` 模块在 PC 上**精确模拟**嵌入式内存环境：

```c
// 设置 256MB 上限（对标 RV1106）
mem_sim_init(256);

// 尝试加载 LLM（200MB）
if (mem_sim_alloc(200, MODEL_ID_LLM) == 0) {
    // 加载成功
}

// 尝试同时加载 OCR（50MB）
if (mem_sim_alloc(50, MODEL_ID_OCR) == 0) {
    // 90% 阈值未触发，加载成功
}

// 内存接近阈值时自动卸载闲置模型
mem_sim_unload_idle();
```

> **注**：`hardware_sim` 模块接口已实现，UI 接入和性能报表生成待开发。

---

## 文档导航

| 文档 | 说明 |
|------|------|
| [EdgeSim_Design.md](EdgeSim_Design.md) | 完整需求与架构设计文档 |
| [business/hardware_sim/hardware_sim.h](business/hardware_sim/hardware_sim.h) | 内存仿真接口 |
| [business/multi_proc/multi_proc.h](business/multi_proc/multi_proc.h) | 多进程管道接口 |
| [business/sqlite_db/sqlite_db.h](business/sqlite_db/sqlite_db.h) | 数据库接口 |
| [ai_engine/llm_engine/llm_engine.h](ai_engine/llm_engine/llm_engine.h) | LLM 引擎接口 |
| [ai_engine/ocr_engine/ocr_engine.h](ai_engine/ocr_engine/ocr_engine.h) | OCR 引擎接口 |
| [ai_engine/asr_engine/asr_engine.h](ai_engine/asr_engine/asr_engine.h) | ASR 引擎接口 |
| [ui_lvgl/ui_lvgl.h](ui_lvgl/ui_lvgl.h) | UI 表现层接口 |

每个文件都带**超详细中文注释**，逐行解释作用、参数、返回值、易错点，适合新手学习。

---

## 常见问题 (FAQ)

### Q1: 为什么不用 Qt / WebUI？

LVGL 内存占用更低（适合 128MB 开发板），纯 C 实现，与核心业务同语言，是嵌入式行业标配。

### Q2: OCR 识别准确率如何？

使用 PP-OCRv3 ONNX 模型在 CPU 上推理，测试图片（李白《静夜思》，1920x1920）识别准确率约 88%（25 字中 22 字正确）。主要优化点包括：

- det 检测用 Otsu 自适应阈值（替代固定阈值）
- rec 输出形状解析支持多维（`out_shape[dim-2]` 取 time_steps）
- 字典索引 0 手动添加 CTC blank
- 检测框按高度 20% 自适应膨胀

### Q4: 真的完全离线吗？

是的。所有推理（LLM / OCR / ASR）均在本地 CPU 上完成，**全程零网络请求**。模型文件在程序启动时一次性加载到内存，运行期间不访问网络。

### Q5: 如何贡献代码？

1. Fork 本仓库
2. 创建特性分支：`git checkout -b feature/your-feature`
3. 提交前确保 `mkdir build && cd build && cmake .. && make` 编译通过
4. 提交 PR，描述清楚修改内容与对应设计文档章节

---

## 开源协议

[MIT License](LICENSE) - 无商用限制，可自由修改与分发。

---

## 致谢

EdgeSim 站在以下开源项目肩膀上：

- [llama.cpp](https://github.com/ggerganov/llama.cpp) - LLM 推理引擎
- [whisper.cpp](https://github.com/ggerganov/whisper.cpp) - ASR 语音识别
- [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) - OCR 文字识别
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) - 跨平台推理引擎
- [LVGL](https://lvgl.io/) - 嵌入式图形库
- [SQLite](https://sqlite.org/) - 嵌入式数据库
- [OpenCV](https://opencv.org/) - 计算机视觉库

感谢这些项目的作者与社区贡献者。
