# =============================================================================
# EdgeSim 根目录 CMakeLists.txt
# =============================================================================
# 【文件作用】
#   这是整个 EdgeSim 项目的 CMake 构建入口。它本身不直接编译任何 .c 文件，
#   而是按设计文档要求的"三层解耦架构"顺序，依次 add_subdirectory 各模块
#   子目录，由每个模块自己的 CMakeLists.txt 完成编译生成静态库，最后把所有
#   静态库链接成可执行文件 edgesim。
#
# 【设计文档硬性约束】
#   1. 三层目录固定：ui_lvgl(表现层) / business(业务层) / ai_engine(引擎层)
#   2. 禁止跨层调用（表现层不能直接调 AI 引擎层）
#   3. 每个模块编译本目录 .c 文件生成 .a 静态库
#
# 【标准编译流程（用户操作）】
#   1. mkdir build              # 新建构建目录（隔离源码与产物）
#   2. cd build                 # 进入构建目录
#   3. cmake ..                 # 读取上级 CMakeLists.txt，生成构建文件
#   4. make                     # 执行构建文件完成编译（等效 cmake --build .）
#   5. ./edgesim                # 运行（产物默认在 build/ 下）
#
# 【交叉编译流程（ARM64）】
#   1. mkdir build-arm64 && cd build-arm64
#   2. cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake ..
#   3. make
#
# 【第三方库自动适配说明】
#   CMake 通过 find_package 自动检测系统已安装的库：
#     - SDL2     ：UI 桌面模拟器依赖（x86 必需，ARM64 可关闭）
#     - SQLite3  ：数据库依赖（必需）
#     - OpenCV   ：OCR 图像预处理（可选，自动检测）
#     - ONNX RT  ：OCR ONNX 推理后端（可选，自动检测）
#   预留扩展位（当前仅声明选项，不实际链接）：
#     - RKNPU    ：瑞芯微 NPU 后端（后续填入路径即用）
#     - TensorRT ：NVIDIA 推理后端（后续填入路径即用）
# =============================================================================


# -----------------------------------------------------------------------------
# 一、CMake 最低版本要求
# -----------------------------------------------------------------------------
# cmake_minimum_required 指定本工程需要的 CMake 最低版本。
# 【阶段 0 更新：3.10 → 3.16，原因如下】
#   1. 各模块 CMakeLists.txt 使用了 file(GLOB ... CONFIGURE_DEPENDS) 语法，
#      该特性要求 CMake 3.12+，在 3.10 下会报"未知参数"错误。
#   2. Ubuntu 20.04 LTS 自带 CMake 3.16.3、Ubuntu 22.04 自带 3.22，
#      与 README 要求的 "Ubuntu 20.04+" 环境完全匹配，无需用户额外升级。
# 低于此版本时 CMake 会直接报错退出，避免后续语法不兼容。
cmake_minimum_required(VERSION 3.16)


# -----------------------------------------------------------------------------
# 二、项目声明
# -----------------------------------------------------------------------------
# project 声明项目名与支持的编程语言。
# LANGUAGES C CXX 表示本项目同时使用 C 和 C++（llama.cpp/whisper.cpp 需要 C++）。
# VERSION 后的版本号可供子模块通过 PROJECT_VERSION 引用。
project(EdgeSim LANGUAGES C CXX VERSION 1.0.0)


# -----------------------------------------------------------------------------
# 三、C / C++ 标准设置
# -----------------------------------------------------------------------------
# C  标准设为 C99，保证代码在各类编译器上可移植。
# C++ 标准设为 C++11（llama.cpp / whisper.cpp 依赖该标准）。
# CMAKE_CXX_STANDARD 11        ：指定 C++11 标准
# CMAKE_CXX_STANDARD_REQUIRED  ：强制要求编译器支持 C++11（否则报错而非降级）
# CMAKE_CXX_EXTENSIONS OFF     ：禁用编译器扩展（如 gnu++11），用纯 c++11
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# -----------------------------------------------------------------------------
# 四、全局编译选项
# -----------------------------------------------------------------------------
# 统一编译参数：-Wall -O2 -g
# -Wall ：开启所有常用警告，便于发现潜在 bug
# -O2   ：二级优化，兼顾编译速度与运行性能
# -g    ：生成调试信息，配合 gdb 调试
# 注意：CMAKE_C_FLAGS / CMAKE_CXX_FLAGS 对所有 target 生效（全局）。
#       也可用 target_compile_options 对单个 target 设置（模块级）。
#       这里用全局方式，保证各模块编译参数一致。
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2 -g")


# -----------------------------------------------------------------------------
# 五、构建类型与输出目录
# -----------------------------------------------------------------------------
# CMAKE_BUILD_TYPE 控制优化级别，未指定时默认空（用上面的 -O2）。
# 常见取值：Debug / Release / RelWithDebInfo / MinSizeRel
# 这里默认设为 Debug，同时保留上面的 -O2 -g 编译参数。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "构建类型" FORCE)
endif()

# 统一输出目录：所有可执行文件与静态库放到 build/bin 与 build/lib
# 便于打包与部署。
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# -----------------------------------------------------------------------------
# 六、用户可配置选项（option）
# -----------------------------------------------------------------------------
# option(变量名 "说明文字" 默认值)
# 用户可通过命令行覆盖：cmake -DBUILD_TESTS=OFF ..

# 6.1 是否编译各模块的 test_* 测试程序
option(BUILD_TESTS "编译各模块的 test_* 测试程序" ON)

# 6.2 是否链接最终 edgesim 可执行文件
#     main.c 位于项目根目录，负责编排 4 进程架构（proc_init + ui_init/ui_loop）。
#     交叉编译 ARM64 且暂不需要完整可执行文件时可设 OFF，仅生成静态库与测试程序。
option(BUILD_EDGESIM "链接最终 edgesim 可执行文件（main.c 在根目录）" ON)

# 6.3 是否禁用 SDL2（ARM64 交叉编译/开发板/Termux 时使用）
option(NO_SDL "禁用 SDL2 桌面依赖（ARM64/开发板/Termux 设为 ON）" OFF)

# 6.4 AI 引擎后端开关
#     默认 OFF = 编译 mock 桩（不依赖第三方库，仅测试接口连通性）
option(WITH_LLAMA    "启用真实 llama.cpp（LLM 推理）"     OFF)
option(WITH_WHISPER  "启用真实 whisper.cpp（ASR 语音）"   OFF)
option(WITH_RKNN     "启用真实 RKNN Lite（OCR，Rockchip NPU）" OFF)

# 6.5 新增第三方库后端（自动检测，也可手动覆盖）
#     AUTO 表示：CMake 用 find_package 自动查找，找到则启用，找不到则跳过。
#     手动指定：cmake -DWITH_OPENCV=ON ..
# 【阶段 1 修复：不用 option() 定义 AUTO 默认值】
#   原因：option() 只接受 ON/OFF 语义，传入 AUTO 会被 CMake 当作"假值"
#         （真值判断为假），缓存变量实际存成 OFF，导致自动检测永远不触发，
#         表现为 WITH_OPENCV:BOOL=OFF、OpenCV 检测分支被跳过。
#   修复：改用 set(变量 "AUTO" CACHE STRING)，保留字符串语义，
#         if(WITH_OPENCV STREQUAL "AUTO") 字符串比较才能正确命中。
#   set_property 仅用于 CMake 命令行补全提示（Tab 键候选值），不影响逻辑。
set(WITH_OPENCV "AUTO" CACHE STRING "启用 OpenCV（OCR 图像预处理）：ON/OFF/AUTO")
set(WITH_ONNXRUNTIME "AUTO" CACHE STRING "启用 ONNX Runtime（OCR 推理后端）：ON/OFF/AUTO")
set_property(CACHE WITH_OPENCV PROPERTY STRINGS ON OFF AUTO)
set_property(CACHE WITH_ONNXRUNTIME PROPERTY STRINGS ON OFF AUTO)

# 6.6 预留扩展位（当前不实际链接，仅声明选项供后续填入）
option(WITH_RKNPU    "预留：瑞芯微 RKNPU 后端（后续启用）"   OFF)
option(WITH_TENSORRT "预留：NVIDIA TensorRT 后端（后续启用）" OFF)


# -----------------------------------------------------------------------------
# 七、第三方库路径变量
# -----------------------------------------------------------------------------
# 这些变量由用户通过命令行传入，指向第三方库源码根目录。
# CMake 用 CACHE PATH 让变量持久化到 build/CMakeCache.txt，避免重复输入。
# 用 CACHE PATH 而非普通 set：下次 cmake 时自动读取缓存值，无需重新传入。

# 7.1 llama.cpp 根目录（含 include/llama.h 与 build/libllama.a）
set(LLAMA_PATH "" CACHE PATH "llama.cpp 根目录（含 include/ 与 build/libllama.a）")

# 7.2 whisper.cpp 根目录（含 include/whisper.h 与 build/libwhisper.a）
set(WHISPER_PATH "" CACHE PATH "whisper.cpp 根目录（含 include/ 与 build/libwhisper.a）")

# 7.3 RKNN SDK 根目录（含 include/rknn_api.h 与 lib/librknnrt.so）
set(RKNN_PATH "" CACHE PATH "RKNN Lite SDK 根目录（含 include/ 与 lib/librknnrt.so）")

# 7.4 stb_image 头文件目录（单头文件库，含 stb_image.h）
set(STB_PATH "" CACHE PATH "stb_image 头文件所在目录（含 stb_image.h）")

# 7.5 LVGL 根目录（含 lvgl.h 与 build/liblvgl.a）
set(LVGL_PATH "" CACHE PATH "LVGL 根目录（含 lvgl.h 与 build/liblvgl.a）")

# 7.5.1 LVGL 静态库路径（可选，交叉编译时用，阶段 5 新增）
#   默认查找顺序：${LVGL_LIBRARY} → ${LVGL_PATH}/build-arm64/lib/liblvgl.a →
#   ${LVGL_PATH}/build/lib/liblvgl.a → ${LVGL_PATH}/build/liblvgl.a
#   交叉编译场景：LVGL_PATH/build 通常是 x86 编译的静态库，架构不匹配
#   （链接时报 "Skipping incompatible file"）。此时应先用 aarch64 工具链
#   编译 LVGL 得到 ARM64 版 liblvgl.a，再用本变量显式指定其路径。
set(LVGL_LIBRARY "" CACHE FILEPATH "LVGL 静态库路径（交叉编译时指定 ARM64 版 liblvgl.a）")


# -----------------------------------------------------------------------------
# 八、系统库自动检测（find_package）
# -----------------------------------------------------------------------------
# find_package 是 CMake 的标准库检测机制：
#   找到 → 创建 IMPORTED target（如 SDL2::SDL2），可直接 target_link_libraries
#   找不到 → 默认报错；加 REQUIRED 才强制；否则用 QUIET 静默

# 8.1 SQLite3（必需）
#     sqlite_db 模块依赖，找不到则编译无法继续
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    # 兜底：部分老系统未提供 SQLite3Config.cmake，用 pkg-config 检测
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SQLITE3 QUIET sqlite3)
    endif()
endif()
if(NOT SQLite3_FOUND)
    message(WARNING
        "[CMake] 未找到 SQLite3 开发库，请安装：\n"
        "  Ubuntu/Debian: sudo apt install libsqlite3-dev\n"
        "  CentOS/RHEL : sudo yum install sqlite-devel\n"
        "  Termux      : pkg install libsqlite3-dev")
endif()

# 8.2 SDL2（条件必需）
#     NO_SDL=ON 时跳过（ARM64 交叉编译场景）
#     x86 桌面开发时必需
if(NOT NO_SDL)
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        # 兜底：用 pkg-config 检测 SDL2
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SDL2 QUIET sdl2)
        endif()
    endif()
    if(NOT SDL2_FOUND)
        # 【阶段 0 增强】SDL2 缺失时给出明确的构建指引
        # 背景：若 BUILD_EDGESIM=ON（要生成最终 edgesim 可执行文件）而 SDL2 缺失，
        #       后续编译 ui_main.c 时会因找不到 <SDL2/SDL.h> 而失败，报错晦涩难懂。
        # 策略：
        #   - BUILD_EDGESIM=ON  → FATAL_ERROR 提前终止配置，并给出两种解决办法
        #   - BUILD_EDGESIM=OFF → 仅 WARNING（只跑模块测试时不强制需要 SDL2）
        if(BUILD_EDGESIM)
            message(FATAL_ERROR
                "[CMake] 未找到 SDL2 开发库，无法链接 edgesim：\n"
                "  请安装：sudo apt install libsdl2-dev\n"
                "  或跳过桌面依赖：cmake -DNO_SDL=ON ..")
        else()
            message(WARNING
                "[CMake] 未找到 SDL2 开发库，仅测试程序可编译：\n"
                "  Ubuntu/Debian: sudo apt install libsdl2-dev\n"
                "  或设置 -DNO_SDL=ON 跳过 SDL2 依赖")
        endif()
    endif()
endif()

# 8.3 OpenCV（可选，自动检测）
#     用于 ocr_engine 模块的图像预处理（读取/缩放/归一化图片）
#     WITH_OPENCV=AUTO 时：找到则定义 HAS_OPENCV 宏并链接，找不到则跳过
set(EDGE_USE_OPENCV OFF)
if(WITH_OPENCV STREQUAL "AUTO" OR WITH_OPENCV STREQUAL "ON")
    find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
    if(OpenCV_FOUND)
        set(EDGE_USE_OPENCV ON)
        message(STATUS "[CMake] OpenCV 已检测到: ${OpenCV_VERSION}（启用 HAS_OPENCV）")
    elseif(WITH_OPENCV STREQUAL "ON")
        message(WARNING
            "[CMake] WITH_OPENCV=ON 但未找到 OpenCV，请安装：\n"
            "  Ubuntu/Debian: sudo apt install libopencv-dev")
    else()
        message(STATUS "[CMake] OpenCV 未安装，跳过（可用 -DWITH_OPENCV=ON 强制要求）")
    endif()
endif()

# 8.4 ONNX Runtime（可选，自动检测）
#     用于 ocr_engine 模块的 ONNX 模型推理
#     ONNX Runtime 无标准 find_package 模块，需手动指定路径
#     优先用环境变量 ONNXRUNTIME_ROOT 或 CMake 变量 ONNXRUNTIME_PATH
set(EDGE_USE_ONNXRUNTIME OFF)
if(WITH_ONNXRUNTIME STREQUAL "AUTO" OR WITH_ONNXRUNTIME STREQUAL "ON")
    # 优先读取 CMake 变量，其次环境变量
    if(NOT ONNXRUNTIME_PATH)
        set(ONNXRUNTIME_PATH "$ENV{ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime 安装目录" FORCE)
    endif()
    if(ONNXRUNTIME_PATH AND EXISTS "${ONNXRUNTIME_PATH}/include/onnxruntime_c_api.h")
        set(EDGE_USE_ONNXRUNTIME ON)
        message(STATUS "[CMake] ONNX Runtime 已检测到: ${ONNXRUNTIME_PATH}（启用 HAS_ONNXRUNTIME）")
    elseif(WITH_ONNXRUNTIME STREQUAL "ON")
        message(WARNING
            "[CMake] WITH_ONNXRUNTIME=ON 但未找到 ONNX Runtime，请设置：\n"
            "  cmake -DONNXRUNTIME_PATH=/path/to/onnxruntime ..")
    else()
        message(STATUS "[CMake] ONNX Runtime 未安装，跳过（可用 -DONNXRUNTIME_PATH=... 启用）")
    endif()
endif()


# -----------------------------------------------------------------------------
# 九、全局包含路径与编译宏（供所有子模块继承）
# -----------------------------------------------------------------------------
# 把项目根目录加入全局 include 路径，方便跨模块引用头文件。
# 注意：设计文档要求"禁止跨层调用"，所以各模块仍应只 include 自己目录的头文件，
#       这里加根目录仅作为兜底，不应被滥用。
include_directories(${CMAKE_SOURCE_DIR})


# -----------------------------------------------------------------------------
# 十、编译宏汇总
# -----------------------------------------------------------------------------
# 根据上面的检测结果，把宏定义放到全局变量 EDGE_COMPILE_DEFS 中，
# 各子模块在自己的 CMakeLists.txt 中引用此变量添加 target_compile_definitions。
# 这样集中管理，避免每个模块重复检测。

# 10.0 工具链文件传递的平台宏（ARM64 交叉编译时由 toolchain-arm64.cmake 设置）
# 工具链文件把 ARM64 / NO_SDL / TARGET_T153 / TARGET_RV1106 / TERMUX 等宏
# 存入 EDGE_TOOLCHAIN_DEFS 变量，这里读取并合并到全局宏列表。
# 这些是平台级宏，需要全局应用到所有模块，因此同时加入 EDGE_PLATFORM_DEFS。
if(EDGE_TOOLCHAIN_DEFS)
    foreach(def ${EDGE_TOOLCHAIN_DEFS})
        list(APPEND EDGE_COMPILE_DEFS ${def})
        list(APPEND EDGE_PLATFORM_DEFS ${def})
    endforeach()
    # 工具链文件设置了 NO_SDL 时，同步更新本项目的 NO_SDL 变量
    # 这样根 CMakeLists.txt 的 SDL2 检测逻辑会自动跳过
    if("NO_SDL" IN_LIST EDGE_TOOLCHAIN_DEFS)
        set(NO_SDL ON CACHE BOOL "由工具链文件自动设置" FORCE)
    endif()
    message(STATUS "[CMake] 工具链平台宏已合并: ${EDGE_TOOLCHAIN_DEFS}")
endif()

# 10.1 NO_SDL 宏（ARM64 模式，手动 -DNO_SDL=ON 或工具链自动设置）
# NO_SDL 是平台级宏，需要全局应用（ui_main.c 等源码用 #ifdef NO_SDL 检测）
if(NO_SDL)
    list(APPEND EDGE_COMPILE_DEFS NO_SDL)
    list(APPEND EDGE_PLATFORM_DEFS NO_SDL)
endif()

# 10.2 AI 引擎后端宏
if(WITH_LLAMA AND LLAMA_PATH)
    list(APPEND EDGE_COMPILE_DEFS HAS_LLAMA_CPP)
endif()
if(WITH_WHISPER AND WHISPER_PATH)
    list(APPEND EDGE_COMPILE_DEFS HAS_WHISPER_CPP)
endif()
if(WITH_RKNN AND RKNN_PATH)
    list(APPEND EDGE_COMPILE_DEFS HAS_RKNN_LITE)
endif()

# 10.3 新增后端宏（OpenCV / ONNX Runtime）
if(EDGE_USE_OPENCV)
    list(APPEND EDGE_COMPILE_DEFS HAS_OPENCV)
endif()
if(EDGE_USE_ONNXRUNTIME)
    list(APPEND EDGE_COMPILE_DEFS HAS_ONNXRUNTIME)
endif()

# 10.4 预留扩展位宏（RKNPU / TensorRT，当前仅声明不实际使用）
if(WITH_RKNPU)
    list(APPEND EDGE_COMPILE_DEFS HAS_RKNPU)
    message(STATUS "[CMake] 预留 HAS_RKNPU 宏已启用（请后续补充库链接）")
endif()
if(WITH_TENSORRT)
    list(APPEND EDGE_COMPILE_DEFS HAS_TENSORRT)
    message(STATUS "[CMake] 预留 HAS_TENSORRT 宏已启用（请后续补充库链接）")
endif()

# 把汇总变量提升为 CACHE 变量，子目录可直接读取
set(EDGE_COMPILE_DEFS "${EDGE_COMPILE_DEFS}" CACHE INTERNAL "全局编译宏汇总")

# 全局应用平台级编译宏
# 注意：只全局应用平台宏（NO_SDL / ARM64 / TARGET_* 等），不全局应用后端宏。
# 后端宏（HAS_LLAMA_CPP / HAS_OPENCV 等）由各模块自己的 CMakeLists.txt 通过
# target_compile_definitions 添加，因为模块级会校验头文件/库是否真实存在。
if(EDGE_PLATFORM_DEFS)
    add_compile_definitions(${EDGE_PLATFORM_DEFS})
endif()


# -----------------------------------------------------------------------------
# 十一、架构标识
# -----------------------------------------------------------------------------
# 检测当前编译目标架构：x86_64 或 arm64
# CMAKE_SYSTEM_PROCESSOR 在交叉编译时由工具链文件设置
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(EDGE_ARCH "arm64")
    message(STATUS "[CMake] 目标架构: ARM64（aarch64）")
else()
    set(EDGE_ARCH "x86_64")
    message(STATUS "[CMake] 目标架构: x86_64")
endif()


# -----------------------------------------------------------------------------
# 十二、子模块编译（按依赖顺序）
# -----------------------------------------------------------------------------
# 编译顺序遵循三层依赖关系：
#   ① 业务层（business）—— 不依赖其他层，最先编译
#   ② AI 引擎层（ai_engine）—— 仅封装第三方库，不依赖业务层
#   ③ 表现层（ui_lvgl）—— 依赖业务层接口，最后编译
# add_subdirectory 会进入指定目录执行该目录的 CMakeLists.txt

# 12.1 业务层
add_subdirectory(business/hardware_sim)
add_subdirectory(business/multi_proc)
add_subdirectory(business/sqlite_db)
add_subdirectory(business/rag)   # 阶段 4：本地 RAG 知识库检索

# 12.2 AI 引擎层
add_subdirectory(ai_engine/llm_engine)
add_subdirectory(ai_engine/ocr_engine)
add_subdirectory(ai_engine/asr_engine)

# 12.3 表现层（条件编译：必须提供 LVGL_PATH）
# 【阶段 0 设计决策】
#   ui_lvgl 模块无条件 #include "lvgl.h"，因此编译它必须存在 LVGL 头文件。
#   但"仅跑业务层/AI 引擎测试"（BUILD_EDGESIM=OFF）时，用户可能尚未准备 LVGL，
#   此时不应强制编译 ui_lvgl，否则会因缺 lvgl.h 直接报错、连测试都跑不了。
#   处理策略：
#     - LVGL 头文件存在                                  → 正常编译 ui_lvgl 子模块
#     - LVGL 缺失且要链接 edgesim（BUILD_EDGESIM=ON）   → FATAL_ERROR 提前报错，
#       避免在编译阶段才以晦涩的 "lvgl.h: No such file" 失败
#     - LVGL 缺失且仅跑测试（BUILD_EDGESIM=OFF）        → 跳过 ui_lvgl 子模块，
#       不影响其他模块与测试程序的构建
#   设置 LVGL_PATH 的示例：cmake -DLVGL_PATH=/path/to/lvgl ..
if(LVGL_PATH AND EXISTS "${LVGL_PATH}/lvgl.h")
    add_subdirectory(ui_lvgl)
    set(EDGE_USE_UI ON)
    message(STATUS "[CMake] UI 模块：LVGL 头文件有效（${LVGL_PATH}），编译 ui_lvgl")
else()
    if(BUILD_EDGESIM)
        message(FATAL_ERROR
            "[CMake] 需要链接 edgesim 但未找到 LVGL 头文件：\n"
            "  请通过 -DLVGL_PATH=/path/to/lvgl 指定 LVGL 根目录（须包含 lvgl.h）：\n"
            "    cmake -DLVGL_PATH=\$HOME/lvgl ..\n"
            "  或仅编译模块测试：cmake -DBUILD_EDGESIM=OFF ..")
    else()
        message(STATUS "[CMake] 未设置 LVGL_PATH（或路径无效），跳过 ui_lvgl 子模块")
    endif()
endif()


# -----------------------------------------------------------------------------
# 十三、链接最终可执行文件 edgesim
# -----------------------------------------------------------------------------
# 创建 edgesim 可执行文件，链接所有模块静态库 + 系统库。
#
# main.c 位于项目根目录，负责：
#   1. proc_set_child_handler 注册子进程入口回调
#   2. proc_init fork 出 LLM/OCR/ASR 3 个子进程
#   3. 父进程调 ui_init / ui_loop / ui_deinit
#   4. 子进程进入 child_worker_loop 收任务→调引擎→回结果
# 若暂不需要完整可执行文件（如仅跑模块测试），可 -DBUILD_EDGESIM=OFF 跳过链接。
if(BUILD_EDGESIM)

    # 收集所有模块静态库名（target 名，CMake 自动加 lib 前缀与 .a 后缀）
    set(EDGE_ALL_LIBS
        hardware_sim     # 业务层：内存仿真
        multi_proc       # 业务层：多进程管道
        sqlite_db        # 业务层：数据库
        rag              # 业务层：RAG 知识库检索（阶段 4）
        llm_engine       # 引擎层：LLM
        ocr_engine       # 引擎层：OCR
        asr_engine       # 引擎层：ASR
        ui_lvgl          # 表现层：LVGL 界面
    )

    # 创建 edgesim 可执行文件
    # main.c 是项目唯一的主入口，负责编排 4 进程架构：
    #   proc_set_child_handler → proc_init fork 子进程 → 父进程 ui_init/ui_loop
    # 子进程在 proc_init 内部调 child_main 不返回 main
    add_executable(edgesim ${CMAKE_SOURCE_DIR}/main.c)

    # 链接所有模块静态库（CMake 自动处理依赖顺序）
    target_link_libraries(edgesim PRIVATE ${EDGE_ALL_LIBS})

    # 链接系统库
    # -lSDL2    ：UI 桌面模拟器（NO_SDL 模式下不链接）
    # -lsqlite3 ：数据库
    # -lpthread ：POSIX 线程
    # -lm       ：数学库
    # -ldl      ：动态加载（llama.cpp / whisper.cpp 需要 dlopen）
    if(NOT NO_SDL AND SDL2_FOUND)
        if(TARGET SDL2::SDL2)
            target_link_libraries(edgesim PRIVATE SDL2::SDL2)
        else()
            target_link_libraries(edgesim PRIVATE ${SDL2_LIBRARIES})
        endif()
    endif()
    if(SQLite3_FOUND)
        if(TARGET SQLite::SQLite3)
            target_link_libraries(edgesim PRIVATE SQLite::SQLite3)
        else()
            target_link_libraries(edgesim PRIVATE ${SQLITE3_LIBRARIES})
        endif()
    else()
        target_link_libraries(edgesim PRIVATE sqlite3)
    endif()
    target_link_libraries(edgesim PRIVATE pthread m dl)

    # LVGL 静态库（若设置了 LVGL_PATH，链接 liblvgl.a）
    # 【阶段 6 修复：查找顺序与 ui_lvgl/CMakeLists.txt 保持一致，
    #   用 CMAKE_CROSSCOMPILING 区分编译模式，避免 x86 误链 ARM64 库】
    if(LVGL_LIBRARY)
        target_link_libraries(edgesim PRIVATE "${LVGL_LIBRARY}")
    elseif(CMAKE_CROSSCOMPILING AND LVGL_PATH AND EXISTS "${LVGL_PATH}/build-arm64/lib/liblvgl.a")
        target_link_libraries(edgesim PRIVATE "${LVGL_PATH}/build-arm64/lib/liblvgl.a")
    elseif(LVGL_PATH AND EXISTS "${LVGL_PATH}/build/lib/liblvgl.a")
        target_link_libraries(edgesim PRIVATE "${LVGL_PATH}/build/lib/liblvgl.a")
    elseif(LVGL_PATH AND EXISTS "${LVGL_PATH}/build/liblvgl.a")
        target_link_libraries(edgesim PRIVATE "${LVGL_PATH}/build/liblvgl.a")
    endif()

    message(STATUS "[CMake] 将链接 edgesim 可执行文件（含 ${EDGE_ALL_LIBS}）")

endif(BUILD_EDGESIM)


# -----------------------------------------------------------------------------
# 十四、辅助目标与信息打印
# -----------------------------------------------------------------------------

# 14.1 伪目标 all（CMake 默认有 all，这里显式声明确保包含 edgesim）
#      CMake 的 all 目标会自动包含所有 add_executable / add_library，
#      无需额外配置。

# 14.2 打印配置摘要（方便用户确认构建选项）
message("")
message("============================================================")
message("  EdgeSim CMake 配置摘要")
message("============================================================")
message("  项目版本        : ${PROJECT_VERSION}")
message("  目标架构        : ${EDGE_ARCH}")
message("  构建类型        : ${CMAKE_BUILD_TYPE}")
message("  构建目录        : ${CMAKE_BINARY_DIR}")
message("  ---- 编译选项 ----")
message("  BUILD_TESTS     : ${BUILD_TESTS}")
message("  BUILD_EDGESIM   : ${BUILD_EDGESIM}")
message("  NO_SDL          : ${NO_SDL}")
message("  ---- 表现层 ----")
message("  LVGL_PATH       : ${LVGL_PATH}  (UI编译=${EDGE_USE_UI})")
message("  ---- AI 引擎后端 ----")
message("  WITH_LLAMA      : ${WITH_LLAMA}  (路径: ${LLAMA_PATH})")
message("  WITH_WHISPER    : ${WITH_WHISPER}  (路径: ${WHISPER_PATH})")
message("  WITH_RKNN       : ${WITH_RKNN}  (路径: ${RKNN_PATH})")
message("  ---- OCR 扩展后端 ----")
message("  OpenCV          : ${EDGE_USE_OPENCV}  (WITH_OPENCV=${WITH_OPENCV})")
message("  ONNX Runtime    : ${EDGE_USE_ONNXRUNTIME}  (WITH_ONNXRUNTIME=${WITH_ONNXRUNTIME})")
message("  RKNPU (预留)    : ${WITH_RKNPU}")
message("  TensorRT (预留) : ${WITH_TENSORRT}")
message("  ---- 全局编译宏 ----")
message("  EDGE_COMPILE_DEFS : ${EDGE_COMPILE_DEFS}")
message("  ---- 第三方库检测 ----")
message("  SQLite3         : ${SQLite3_FOUND}")
message("  SDL2            : ${SDL2_FOUND}")
message("  OpenCV          : ${OpenCV_FOUND}")
message("  LVGL_PATH       : ${LVGL_PATH}")
message("============================================================")
message("")

# 14.3 install 规则（可选，预留）
#      真实部署可用 cmake --install 或 make install 把 edgesim 安装到系统路径
# install(TARGETS edgesim DESTINATION bin)
