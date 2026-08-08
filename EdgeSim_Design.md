# EdgeSim 项目完整需求与架构设计文档
## 项目概述
### 项目名称
EdgeSim
### 项目定位
1. 面向普通用户：纯离线本地AI工具，无网络、不上传数据，低配电脑/安卓手机可用，支持对话、OCR、离线语音、私有知识库。
2. 面向嵌入式开发者（核心差异化亮点）：内置硬件内存仿真器，在PC模拟T153/RV1106低成本Linux开发板小内存环境；三层解耦代码可直接移植嵌入式硬件，完整C/C++端侧AI开源工程。
### 核心特色
市面稀缺：离线AI客户端 + 嵌入式硬件仿真二合一；纯C/C++、LVGL原生界面、CMake统一构建。

## 一、完整需求清单
### 1.1 普通用户功能需求
1. 离线大模型对话
- 加载INT8量化Qwen1.8B，全程断网运行，无任何网络请求。
- 长对话持久化保存，支持文案创作、翻译、解题、笔记总结。
2. 本地私有RAG知识库
- 支持导入TXT/MD/PDF/图片，仅基于本地文件回答，保护隐私数据。
3. RKNN轻量化OCR文字识别
- 支持截图、照片上传，提取图片内文字，兼容手写笔记识别。
4. Whisper离线语音交互
- 麦克风录音转文字，语音提问、语音备忘录、语音翻译。
5. LVGL图形可视化界面
- 主对话窗口、文件导入面板、硬件仿真控制面板、桌面悬浮快捷小窗口。
- 虚拟机器人动画交互、弹窗操作提示。

### 1.2 嵌入式开发者专属功能（项目核心竞争力）
1. 硬件内存仿真管控
- 自定义全局内存上限：256MB / 512MB / 1GB，对标低成本Linux开发板。
- 实时统计总内存占用、各AI模型独立内存占用。
- 内存接近阈值时自动卸载闲置AI模型，防止低端设备OOM崩溃。
2. 多AI并发压力测试
- 同时启动LLM+OCR+语音三大推理任务，模拟小型机器人多负载运行场景。
3. 自动化性能报表生成
- 输出报告：模型量化前后体积、单轮推理延迟、内存峰值、长时间并发稳定性日志。
4. 嵌入式硬件移植支撑
- 三层完全解耦架构，仅修改外设驱动代码即可移植到T153/RV1106开发板。
- 配套ARM64交叉编译CMake工具链文件（toolchain-arm64.cmake），一键生成安卓Termux、开发板可执行程序。

### 1.3 底层系统支撑需求
1. 4进程隔离架构
- 进程划分：UI主进程、LLM推理子进程、OCR识别子进程、ASR语音子进程。
- 使用匿名管道完成进程间双向IPC通信。
2. 异常容错机制
- 捕获子进程崩溃信号，自动重启故障子进程；自动回收僵尸进程。
- 互斥锁、信号量处理多进程资源竞争，避免数据错乱。
3. 本地持久化存储
- SQLite数据库存储：对话历史、知识库文件索引、性能测试日志。
4. 多平台编译与打包
- x86 Linux：CMake一键编译（阶段0起唯一构建方式），打包AppImage单文件，双击直接运行。
- Windows：提供WSL一键部署Shell脚本（WSL2 内执行同一套 CMake 流程）。
- ARM64 Linux：CMake 工具链文件条件交叉编译，适配安卓Termux、全志T153、瑞芯微RV1106。

### 1.4 硬性非功能约束（开发强制遵守，禁止改动）
1. 编程语言：核心业务/底层逻辑全部 C/C++；仅模型量化使用少量Python辅助脚本。
2. 编译构建工具：统一使用 **CMake**（根CMakeLists.txt + 各模块CMakeLists.txt），仓库中禁止出现 Makefile。
3. GUI方案：LVGL + SDL2模拟器，禁止Qt、网页WebUI、HTML前端。
4. 固定第三方依赖库：llama.cpp、whisper.cpp、RKNN Lite、SDL2、SQLite3。
5. 内存规则：程序运行严格遵循用户设置内存上限，超出阈值自动回收闲置模型。
6. 开源协议：MIT开源协议，无商用版权限制。
7. 完成数据持久化测试。

## 二、系统架构设计（三层解耦）
### 2.1 架构分层（严格遵守，禁止跨层调用）
1. 表现层（ui_lvgl）：仅负责界面显示、用户输入，**不处理任何AI推理、内存管理**；通过管道向业务层发指令、收结果。
2. 业务逻辑层（business）：核心控制中心，包含：
   - hardware_sim：内存仿真、上限管控、OOM保护、性能报表
   - multi_proc：4进程fork、管道IPC、信号处理、崩溃重启、僵尸回收
   - sqlite_db：对话/知识库/日志持久化
3. AI引擎层（ai_engine）：仅封装第三方库，提供统一调用接口，**不处理进程、内存调度**：
   - llm_engine：llama.cpp封装
   - ocr_engine：RKNN Lite封装
   - asr_engine：whisper.cpp封装

### 2.2 目录结构（固定，禁止新增/删除一级目录）
EdgeSim/
├── ui\_lvgl/              # LVGL 界面（表现层）
├── business/              # 业务逻辑层
│   ├── hardware\_sim/      # 内存仿真模块
│   ├── multi\_proc/        # 多进程管道模块
│   └── sqlite\_db/         # 数据库封装模块
├── ai\_engine/              # AI 引擎层
│   ├── llm\_engine/
│   ├── ocr\_engine/
│   └── asr\_engine/
├── cross\_compile/         # ARM 交叉编译配置（toolchain-arm64.cmake）
├── scripts/                # 量化、打包、部署脚本
├── CMakeLists.txt          # 根目录 CMake 构建入口（唯一）
└── EdgeSim\_Design.md       # 本设计文档

## 三、核心模块详细设计
### 3.1 hardware_sim（内存仿真）
- 全局变量：g_total_mem_limit（默认512MB）、g_current_used_mem
- 核心接口：
  - mem_sim_init(limit_mb)：初始化内存上限
  - mem_sim_alloc(size_mb, model_id)：申请内存，超限返回失败
  - mem_sim_free(model_id)：释放指定模型内存
  - mem_sim_get_usage()：获取当前占用、剩余内存
  - mem_sim_unload_idle()：卸载最闲置模型，回收内存
  - mem_sim_gen_report()：生成性能报表
- 实现要求：实时统计、精确到MB、接近阈值（90%）自动触发回收。

### 3.2 multi_proc（多进程+管道）
- 进程数组：proc[4] = {UI, LLM, OCR, ASR}
- 数据结构：TaskData（cmd、data_len、data_buf、model_id、timestamp）
- 核心接口：
  - proc_init()：fork4进程、创建双向匿名管道
  - proc_send(proc_id, data)：向指定进程发数据
  - proc_recv(proc_id, data)：从指定进程收数据
  - proc_monitor()：捕获SIGCHLD、SIGSEGV，自动重启崩溃子进程
  - proc_cleanup()：回收僵尸进程、关闭管道
- 实现要求：管道非阻塞、读写超时、信号安全、无死锁。

### 3.3 sqlite_db（数据库）
- 表设计：
  - chat_history(id, time, role, content)
  - kb_files(id, path, type, create_time)
  - perf_log(id, time, model, latency, mem_peak)
- 核心接口：db_init()、chat_save()、chat_query()、kb_add()、perf_log()

### 3.4 AI引擎（统一接口）
- 统一函数：int xxx_engine_init(const char* model_path)
- 统一推理：int xxx_engine_run(const char* input, char* output)
- 统一销毁：void xxx_engine_destroy()
- 实现要求：仅封装，不改造第三方库，不处理进程/内存。

### 3.5 ui_lvgl（界面）
- 窗口：主对话窗口、文件导入弹窗、内存监控面板、悬浮小窗口
- 控件：输入框、发送按钮、文件选择器、内存进度条、日志文本域
- 通信：界面按钮→发管道指令给业务层→业务层调用AI引擎→结果回管道→界面显示
- 实现要求：纯UI逻辑，无AI代码，LVGL控件自适应。

## 四、编译与部署设计
### 4.1 CMake 构建规则（阶段0起唯一构建方式）
- 根CMakeLists.txt：`mkdir build && cd build && cmake .. && make`（编译x86），
  `rm -rf build`（清理），`cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake ..`（交叉编译ARM64），
  AppImage 打包仍由 scripts/build_appimage.sh 完成。
- 子CMakeLists.txt：每个模块一个，编译本目录 .c 文件生成 .a 静态库（file(GLOB) 自动收集，排除 test_*）。
- 编译选项（option）集中管理：BUILD_TESTS / BUILD_EDGESIM / NO_SDL / WITH_LLAMA / WITH_WHISPER / WITH_RKNN / WITH_OPENCV / WITH_ONNXRUNTIME。

### 4.2 交叉编译
- 工具链：aarch64-linux-gnu-
- 条件编译：ARM64下禁用SDL2桌面依赖，适配Termux/开发板

### 4.3 打包脚本
- scripts/quantize.sh：调用Python脚本量化Qwen1.8B为INT8
- scripts/build_appimage.sh：打包x86为AppImage
- scripts/termux_deploy.sh：一键部署到安卓Termux

## 五、性能与稳定性要求
1. 内存：256MB上限可稳定运行LLM+OCR（ASR按需启动）
2. 延迟：LLM单轮≤2s，OCR≤1s，ASR≤3s
3. 稳定性：连续运行72h无崩溃、无内存泄漏
4. 移植性：T153/RV1106编译通过，可执行

## 六、开发优先级顺序（必须按此执行）
1. 搭建目录骨架 + 根CMakeLists.txt（最先做，CMake 唯一构建体系）
2. 开发business/hardware_sim（内存仿真，核心）
3. 开发business/multi_proc（多进程管道，核心）
4. AI引擎层封装：依次开发llm_engine、ocr_engine、asr_engine，统一对外调用接口，单独测试推理功能。
5. UI界面开发：编写ui_lvgl全部窗口、交互控件，绑定管道与业务层通信逻辑。
6. 交叉编译与打包脚本：完善cross_compile配置、scripts内打包/部署/量化Shell脚本。
7. 极限压力测试：256MB内存上限并发运行三大AI，修复内存泄漏、管道阻塞、进程崩溃bug。
8. 代码统一优化：规范变量命名、删减冗余代码、补充完整中文注释。
9. 开源交付：编写GitHub README、架构说明、开发板移植教程，上传仓库开源。

## 七、代码开发硬性规则
1. 本文档为唯一开发标准，所有代码、模块、目录不得脱离文档设计。
2. 底层核心代码（内存仿真、多进程IPC）必须逐行阅读、编译自测，禁止直接复用AI生成代码不调试。
3. 界面、Shell脚本、CMakeLists.txt 模板可交给AI生成，但生成后核对是否符合文档约束。
4. 全程杜绝引入 Makefile、Qt、WebUI、Python核心逻辑，出现违规代码立即重写。
