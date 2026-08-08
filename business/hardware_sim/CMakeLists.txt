# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— business/hardware_sim（内存仿真模块）
# =============================================================================
# 【模块定位】
#   业务逻辑层核心模块，负责：
#   - 全局内存上限管控（256MB / 512MB / 1GB 可配置）
#   - 各 AI 模型内存占用实时统计
#   - 接近阈值（90%）自动卸载闲置模型，防止 OOM 崩溃
#   - 生成性能报表（内存占用、推理延迟、内存峰值）
#
# 【本 CMakeLists.txt 作用】
#   1. 编译 hardware_sim.c → libhardware_sim.a 静态库
#   2. 可选编译 test_hardware_sim 测试程序（BUILD_TESTS=ON 时）
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
# file(GLOB ...) 扫描当前目录匹配模式的文件，结果存入变量。
# CONFIGURE_DEPENDS：源文件增删后下次 cmake 自动重新扫描（CMake 3.12+）。
# 排除 test_*.c：避免测试文件的 main() 被编入静态库导致链接冲突。
file(GLOB HW_SIM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
# 从源文件列表中过滤掉 test_ 开头的测试文件
list(FILTER HW_SIM_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libhardware_sim.a）
# -----------------------------------------------------------------------------
# add_library 创建库目标：
#   hardware_sim  ：目标名（CMake 自动加 lib 前缀与 .a/.so 后缀）
#   STATIC        ：静态库
#   ${HW_SIM_SOURCES}：源文件列表
# 如果源文件列表为空，用 add_library(hardware_sim STATIC) 创建空库。
if(HW_SIM_SOURCES)
    add_library(hardware_sim STATIC ${HW_SIM_SOURCES})
else()
    add_library(hardware_sim STATIC)
endif()

# 头文件作为 INTERFACE 依赖（让引用本库的目标自动获得 include 路径）
# target_include_directories 的 INTERFACE 关键字：
#   只在别人依赖本库时传递 include 路径，本库自己编译不需要（因为就在本目录）
target_include_directories(hardware_sim INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接 pthread
# hardware_sim.c 使用 pthread_mutex_* 互斥锁保护内部状态
target_link_libraries(hardware_sim PUBLIC pthread)


# -----------------------------------------------------------------------------
# 三、测试程序
# -----------------------------------------------------------------------------
# BUILD_TESTS=ON 时编译 test_hardware_sim.c 为独立可执行文件
# 测试程序链接本模块源文件（不用静态库，避免路径问题）
if(BUILD_TESTS)
    file(GLOB HW_SIM_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${HW_SIM_TESTS})
        # 从 test_hardware_sim.c 提取 test_hardware_sim 作为可执行文件名
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        # 测试程序需要 hardware_sim.c 的实现，用源码方式编译进来
        target_sources(${test_name} PRIVATE ${HW_SIM_SOURCES})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(${test_name} PRIVATE pthread)
        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()


# -----------------------------------------------------------------------------
# 四、清理辅助文件（perf_report.txt）
# -----------------------------------------------------------------------------
# CMake 的 clean 目标默认只删 build 产物，不删源码目录文件。
# 这里用 set_property 附加清理 perf_report.txt（hardware_sim 生成的性能报表）。
# 注意：CMake 的 clean 不会删源码目录文件，此设置仅作标记。
# 用户仍需手动删除源码目录下的 perf_report.txt。
