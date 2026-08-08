# =============================================================================
# EdgeSim ARM64 交叉编译 CMake 工具链文件  cross_compile/toolchain-arm64.cmake
# =============================================================================
# 【文件作用】
#   本文件是 CMake 工具链文件（toolchain file），用于 ARM64 交叉编译。
#   它告诉 CMake：目标平台是 ARM64 Linux，使用 aarch64-linux-gnu- 工具链。
#
# 【使用方法】
#   cmake -DCMAKE_TOOLCHAIN_FILE=<本文件路径> <源码目录>
#
#   示例：
#     mkdir build-arm64 && cd build-arm64
#     cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake ..
#     make
#
#   指定目标平台（可选）：
#     cmake -DCMAKE_TOOLCHAIN_FILE=../cross_compile/toolchain-arm64.cmake \
#           -DPLATFORM=RV1106 ..
#
# 【设计文档对应】
#   EdgeSim_Design.md 第 4.2 节「交叉编译」
#   工具链：aarch64-linux-gnu-
#   条件编译：ARM64 下禁用 SDL2 桌面依赖（NO_SDL=ON）
#
# 【目标平台】
#   1. 全志 T153（ARM Cortex-A7 x4，1.2GHz，256MB DDR3）
#   2. 瑞芯微 RV1106（ARM Cortex-A7 x1，1.0GHz，128MB DDR3L，带 NPU）
#   3. 安卓 Termux（aarch64，依赖手机内存）
#   三者均使用 aarch64-linux-gnu- 工具链，差异在 CFLAGS 与库路径
# =============================================================================


# -----------------------------------------------------------------------------
# 一、目标平台标识（必须放在 project() 之前）
# -----------------------------------------------------------------------------
# CMAKE_SYSTEM_NAME    ：目标操作系统名称（Linux 表示用 glibc + ELF 二进制）
# CMAKE_SYSTEM_PROCESSOR：目标 CPU 架构（aarch64 = 64 位 ARMv8）
# 这两个变量必须在 project() 之前设置，CMake 据此判断是否为交叉编译。
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 目标平台版本（可选，影响部分库的 ABI 判断）
set(CMAKE_SYSTEM_VERSION   1)


# -----------------------------------------------------------------------------
# 二、编译器路径
# -----------------------------------------------------------------------------
# Ubuntu/Debian 安装命令：sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# 安装后编译器位于 /usr/bin/aarch64-linux-gnu-gcc 与 -g++
# 若工具链装在自定义路径，可通过 CROSS_PREFIX 变量覆盖：
#   cmake -DCROSS_PREFIX=/opt/toolchain/bin/aarch64-linux-gnu- ...

# CROSS_PREFIX 默认值（Ubuntu 包安装的 aarch64 工具链前缀）
if(NOT DEFINED CROSS_PREFIX)
    set(CROSS_PREFIX "/usr/bin/aarch64-linux-gnu-")
endif()

# 设置 C / C++ 编译器
set(CMAKE_C_COMPILER   "${CROSS_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_PREFIX}g++")

# 设置静态库归档工具
set(CMAKE_AR           "${CROSS_PREFIX}ar" CACHE FILEPATH "ARM64 归档工具")

# 设置 ranlib（生成静态库索引，加速链接）
set(CMAKE_RANLIB       "${CROSS_PREFIX}ranlib" CACHE FILEPATH "ARM64 ranlib 工具")


# -----------------------------------------------------------------------------
# 三、编译选项
# -----------------------------------------------------------------------------
# 通用选项：
#   -Wall       ：开启常用警告
#   -O2         ：二级优化（兼顾速度与体积）
#   -g          ：调试信息
#   -std=c++11  ：C++ 标准（llama.cpp / whisper.cpp 要求）
set(ARM_COMMON_FLAGS "-Wall -O2 -g")

# ARM 架构专用选项：
#   -march=armv8-a      ：目标架构 ARMv8-A（所有 aarch64 通用）
#   -mtune=cortex-a72   ：针对 Cortex-A72 优化指令调度
#                         （T153/RV1106 实际是 A7，A72 调度对 A7 也能跑，兼容）
# 真实项目可针对具体芯片调优：
#   T153   : -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard（实际是 32 位）
#   RV1106 : -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
# 这里给出 aarch64 通用版，开发板移植时按需调整
set(ARM_ARCH_FLAGS "-march=armv8-a -mtune=cortex-a72")

# 合并到 CMAKE_C_FLAGS / CMAKE_CXX_FLAGS
set(CMAKE_C_FLAGS       "${ARM_COMMON_FLAGS} ${ARM_ARCH_FLAGS}" CACHE STRING "ARM64 C 编译选项" FORCE)
set(CMAKE_CXX_FLAGS     "${ARM_COMMON_FLAGS} ${ARM_ARCH_FLAGS} -std=c++11" CACHE STRING "ARM64 C++ 编译选项" FORCE)


# -----------------------------------------------------------------------------
# 四、平台特征宏
# -----------------------------------------------------------------------------
# 这些宏在源码中用 #ifdef 检测，控制平台相关代码分支：
#   -DARM64     ：当前为 ARM64 编译（业务层可选检测）
#   -DNO_SDL    ：禁用 SDL2 桌面依赖（Termux/开发板无桌面）
# 设计文档 4.2 节：ARM64 下禁用 SDL2 桌面依赖
set(EDGE_PLATFORM_DEFS "ARM64;NO_SDL")

# 通过 CMake 的 add_compile_definitions 全局设置（等效 -D 选项）
# 注意：工具链文件中的 add_compile_definitions 在 project() 后生效
# 所以这里用变量传递，由根 CMakeLists.txt 读取后调用 add_compile_definitions
set(EDGE_TOOLCHAIN_DEFS "${EDGE_PLATFORM_DEFS}" CACHE INTERNAL "工具链平台宏")


# -----------------------------------------------------------------------------
# 五、目标平台特定配置
# -----------------------------------------------------------------------------
# 用法：cmake -DCMAKE_TOOLCHAIN_FILE=... -DPLATFORM=RV1106 ..
# 可选值：T153 / RV1106 / TERMUX
if(DEFINED PLATFORM)
    if(PLATFORM STREQUAL "T153")
        # 全志 T153：32 位 ARMv7，1.2GHz 四核 Cortex-A7
        # T153 实际是 armv7，需要 32 位工具链 arm-linux-gnueabihf-
        # 这里仅作示例，真实 T153 移植需切换到 32 位工具链
        list(APPEND EDGE_TOOLCHAIN_DEFS TARGET_T153)
        message(STATUS "[CROSS] 目标平台: 全志 T153")

    elseif(PLATFORM STREQUAL "RV1106")
        # 瑞芯微 RV1106：单核 Cortex-A7，带 NPU（0.5 TOPS）
        # NPU 通过 RKNN Lite 调用，ocr_engine 需链接 librknnrt.so
        list(APPEND EDGE_TOOLCHAIN_DEFS TARGET_RV1106)
        message(STATUS "[CROSS] 目标平台: 瑞芯微 RV1106（带 NPU）")

    elseif(PLATFORM STREQUAL "TERMUX")
        # 安卓 Termux：使用 clang，路径前缀不同
        # Termux 自带 clang，无需交叉工具链，直接在 Termux 内 cmake
        list(APPEND EDGE_TOOLCHAIN_DEFS TERMUX)
        message(STATUS "[CROSS] 目标平台: 安卓 Termux")

    else()
        message(WARNING "[CROSS] 未知 PLATFORM=${PLATFORM}，使用通用 ARM64 配置")
    endif()
endif()

# 更新缓存
set(EDGE_TOOLCHAIN_DEFS "${EDGE_TOOLCHAIN_DEFS}" CACHE INTERNAL "工具链平台宏（含平台特定）")


# -----------------------------------------------------------------------------
# 六、sysroot 配置（高级，可选）
# -----------------------------------------------------------------------------
# sysroot 是交叉编译的"目标系统根目录镜像"，包含目标板的 /usr/lib /usr/include
# 当目标板与编译机 glibc 版本不一致时，必须用 sysroot 防止 ABI 不兼容
# 常见 sysroot 位置：
#   - Linaro 工具链：/opt/linaro/<version>/sysroot
#   - Buildroot：output/host/aarch64-buildroot-linux-gnu/sysroot
#   - 自建：从开发板拷贝 /usr/lib /lib 到 PC 某目录
#
# 启用方法（取消注释并修改路径，或命令行传入）：
# set(SYSROOT "/opt/aarch64-sysroot" CACHE PATH "ARM64 sysroot 路径")
# if(EXISTS "${SYSROOT}")
#     set(CMAKE_SYSROOT "${SYSROOT}")
#     set(CMAKE_FIND_ROOT_PATH "${SYSROOT}")
# endif()


# -----------------------------------------------------------------------------
# 七、find_package 搜索路径策略（交叉编译专用）
# -----------------------------------------------------------------------------
# 交叉编译时，CMake 的 find_package 默认会搜索宿主机路径，找到的库架构不匹配。
# 需要限制搜索范围，只在 sysroot 与工具链路径下查找。
#
# 【阶段 5 修复：必须设置 CMAKE_FIND_ROOT_PATH】
#   之前只设置了 MODE_* = ONLY 但未设 FIND_ROOT_PATH：ONLY 模式默认
#   仍以 /usr 为根搜索 → 找到 x86 的 sqlite3.h/libsqlite3.so → 链接时报
#   "Skipping incompatible file"（交叉编译失败）。
#   设置 FIND_ROOT_PATH = /usr/aarch64-linux-gnu（Ubuntu 多架构默认目录，
#   用户用 env_arm64.sh/源码编译的 ARM64 库放这里）后，配合 MODE_* = ONLY，
#   find_package(SQLite3) 只在此目录搜索 ARM64 版本。
#   若用户的 ARM64 库在其他路径，可追加（分号分隔）：
#     -DCMAKE_FIND_ROOT_PATH="/usr/aarch64-linux-gnu;/opt/arm64-libs"
set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu" CACHE PATH
    "ARM64 交叉库/头文件根目录（Ubuntu 多架构默认位置，可追加）")
#
# CMAKE_FIND_ROOT_PATH_MODE_* 取值：
#   NEVER    ：不在目标平台路径搜索（只在宿主机搜索）
#   ONLY     ：只在目标平台路径搜索（推荐用于库）
#   BOTH     ：两处都搜（默认，宿主机优先）
#
# 推荐配置：
#   程序（编译器等）：NEVER（用宿主机的）
#   库：ONLY（用目标平台的）
#   头文件：ONLY（用目标平台的）
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)


# -----------------------------------------------------------------------------
# 八、平台信息打印（便于确认交叉编译配置是否正确）
# -----------------------------------------------------------------------------
message(STATUS "============================================================")
message(STATUS "  EdgeSim ARM64 交叉编译工具链")
message(STATUS "============================================================")
message(STATUS "  工具链前缀    : ${CROSS_PREFIX}")
message(STATUS "  C 编译器      : ${CMAKE_C_COMPILER}")
message(STATUS "  C++ 编译器    : ${CMAKE_CXX_COMPILER}")
message(STATUS "  归档工具      : ${CMAKE_AR}")
message(STATUS "  目标系统      : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  架构标志      : ${ARM_ARCH_FLAGS}")
message(STATUS "  平台宏        : ${EDGE_TOOLCHAIN_DEFS}")
if(DEFINED PLATFORM)
    message(STATUS "  目标平台      : ${PLATFORM}")
endif()
message(STATUS "============================================================")
