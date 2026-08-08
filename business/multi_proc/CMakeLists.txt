# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— business/multi_proc（多进程管道模块）
# =============================================================================
# 【模块定位】
#   业务逻辑层核心模块，负责：
#   - 4 进程隔离架构：UI 主进程 + LLM/OCR/ASR 子进程
#   - 双向匿名管道 IPC（pipe + select 非阻塞）
#   - SIGCHLD 信号捕获 + 崩溃自动重启 + 僵尸回收
#
# 【本 CMakeLists.txt 作用】
#   1. 编译 multi_proc.c → libmulti_proc.a 静态库
#   2. 可选编译 test_multi_proc 测试程序（BUILD_TESTS=ON 时）
#
# 【设计文档约束】
#   每个目录一个子构建文件，仅编译本目录源文件，不跨目录引用
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
file(GLOB MULTI_PROC_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER MULTI_PROC_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libmulti_proc.a）
# -----------------------------------------------------------------------------
# 本模块仅用 libc（fork/pipe/select/signal/waitpid 均在标准 C 库），
# 无需额外链接第三方库。
if(MULTI_PROC_SOURCES)
    add_library(multi_proc STATIC ${MULTI_PROC_SOURCES})
else()
    add_library(multi_proc STATIC)
endif()

# 头文件路径对外暴露（ui_lvgl 等模块依赖 multi_proc.h）
target_include_directories(multi_proc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# -----------------------------------------------------------------------------
# 三、测试程序
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    file(GLOB MULTI_PROC_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${MULTI_PROC_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        # 测试程序用源码方式编译进来（不链接静态库）
        target_sources(${test_name} PRIVATE ${MULTI_PROC_SOURCES})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()
