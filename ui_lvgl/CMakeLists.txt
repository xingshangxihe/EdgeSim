# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— ui_lvgl（LVGL 图形界面表现层）
# =============================================================================
# 【模块定位】
#   表现层模块，仅负责界面显示与用户输入，禁止处理任何 AI 推理或内存管理：
#   - 主对话窗口、文件导入弹窗、内存监控面板、桌面悬浮小窗口
#   - 控件：输入框、发送按钮、文件选择器、内存进度条、日志文本域
#   - 通信：界面按钮 → 发管道指令给业务层 → 业务层调用 AI 引擎 → 结果回管道 → 界面显示
#
# 【条件编译说明】
#   默认 x86 模式：链接 SDL2 + LVGL，可在 PC 上运行
#   交叉编译：-DNO_SDL=ON，跳过 SDL2，仅生成静态库供子进程调用
#
# 【依赖说明】
#   - LVGL：v8.2+，需设置 LVGL_PATH 指向 LVGL 根目录
#     （包含 lvgl.h 头文件与 build/liblvgl.a 静态库）
#   - SDL2：通过 find_package 自动获取（NO_SDL=ON 时跳过）
#   - multi_proc：ui_pipeline.c 依赖 multi_proc.h（头文件路径自动传递）
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
# ui_lvgl 模块的源文件列表：
#   ui_main.c            —— UI 主入口（SDL2 初始化 + LVGL 注册 + 主循环）
#   ui_chat_window.c     —— 对话窗口（消息列表 + 输入框 + 发送按钮）
#   ui_file_import.c     —— 文件导入面板（知识库文件选择）
#   ui_mem_monitor.c     —— 内存监控面板（进度条 + 模型占用统计）
#   ui_floating_window.c —— 桌面悬浮小窗口（最小化时的快捷入口）
#   ui_pipeline.c        —— 管道桥接（proc_send/recv 与 LVGL timer 集成）
file(GLOB UI_LVGL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER UI_LVGL_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libui_lvgl.a）
# -----------------------------------------------------------------------------
if(UI_LVGL_SOURCES)
    add_library(ui_lvgl STATIC ${UI_LVGL_SOURCES})
else()
    add_library(ui_lvgl STATIC)
endif()

# 头文件路径对外暴露
target_include_directories(ui_lvgl INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 检测中文字体文件是否存在，存在则定义 HAS_CHINESE_FONT 宏
# 字体文件由 lv_font_conv 工具生成（含 ASCII + 常用汉字）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lv_font_chinese_16.c")
    target_compile_definitions(ui_lvgl PUBLIC HAS_CHINESE_FONT)
    message(STATUS "[CMake] UI 模块：检测到中文字体 lv_font_chinese_16.c，启用中文显示")
else()
    message(STATUS "[CMake] UI 模块：未找到中文字体文件，使用 montserrat_14（仅英文）")
endif()

# 阶段 4.5：lv_font_conv 生成的字体文件头部 include 兼容处理。
#   若生成的字体文件是 #include "lvgl/lvgl.h"（带 lvgl/ 前缀，老版本
#   lv_font_conv 默认），则需 LVGL 仓库父目录在 include 路径：
#     "${LVGL_PATH}/.."  +  "lvgl/lvgl.h"  ==  "${LVGL_PATH}/lvgl.h"  ✓
#   新脚本已用 --lv-include lvgl.h 生成无前缀版本，此路径仅作兼容兜底。
#   PRIVATE：只影响 ui_lvgl 目标自身的编译（字体文件仅在本库编译），
#   不对外传播，避免污染其他目标的 include 顺序。
if(LVGL_PATH)
    target_include_directories(ui_lvgl PRIVATE "${LVGL_PATH}/..")
endif()


# -----------------------------------------------------------------------------
# 三、业务层头文件依赖
# -----------------------------------------------------------------------------
# ui_pipeline.c 包含 multi_proc.h，需要 business/multi_proc 目录的 include 路径。
# ui_chat_window.c 包含 sqlite_db.h，需要 business/sqlite_db 目录的 include 路径。
# ui_pipeline.c 包含 hardware_sim.h（阶段 2 内存监控），需要 hardware_sim 目录。
# ui_chat_window.c 包含 rag.h（阶段 4 RAG 检索），需要 business/rag 目录。
# 用相对路径指向同级的 business 子目录。
target_include_directories(ui_lvgl PUBLIC
    "${CMAKE_SOURCE_DIR}/business/multi_proc"
    "${CMAKE_SOURCE_DIR}/business/sqlite_db"
    "${CMAKE_SOURCE_DIR}/business/hardware_sim"
    "${CMAKE_SOURCE_DIR}/business/rag"
)

# 阶段 2：ui_pipeline.c 调用 hardware_sim 的 mem_sim_* 接口。
# 阶段 4：ui_chat_window.c 调用 rag_retrieve 检索接口。
# 静态库本身不立即解析符号，但 PUBLIC 传播后，最终链接 edgesim 时可解析；
# 同时自动传递这些模块的头文件路径给所有依赖 ui_lvgl 的目标。
if(TARGET hardware_sim)
    target_link_libraries(ui_lvgl PUBLIC hardware_sim)
endif()
if(TARGET rag)
    target_link_libraries(ui_lvgl PUBLIC rag)
endif()


# -----------------------------------------------------------------------------
# 四、LVGL 头文件与库依赖
# -----------------------------------------------------------------------------
# 检测 LVGL_PATH 变量，添加头文件路径并链接 liblvgl.a。
if(LVGL_PATH AND EXISTS "${LVGL_PATH}/lvgl.h")
    # 添加 LVGL 头文件路径（lvgl.h 在根目录，lv_*.h 在 src/ 子目录）
    target_include_directories(ui_lvgl PUBLIC
        "${LVGL_PATH}"
        "${LVGL_PATH}/src"
    )
    message(STATUS "[CMake] UI 模块：LVGL 头文件路径: ${LVGL_PATH}")

    # 链接 LVGL 静态库（编译后的 liblvgl.a）
    # 【阶段 6 修复：查找顺序必须区分编译模式】
    #   之前无条件优先 build-arm64/，导致 x86 桌面构建误链 ARM64 的
    #   liblvgl.a（链接报 "Relocations in generic ELF (EM: 183)"）。
    #   修复：用 CMAKE_CROSSCOMPILING 区分——
    #     - 交叉编译（工具链文件使其为 TRUE）：优先 build-arm64（ARM64 版）
    #     - x86 桌面：优先 build/（x86 版），不使用 build-arm64
    #   显式 -DLVGL_LIBRARY= 始终最高优先（用户明确指定）。
    if(LVGL_LIBRARY)
        target_link_libraries(ui_lvgl PUBLIC "${LVGL_LIBRARY}")
        message(STATUS "[CMake] UI 模块：链接 LVGL 静态库（显式指定）: ${LVGL_LIBRARY}")
    elseif(CMAKE_CROSSCOMPILING AND EXISTS "${LVGL_PATH}/build-arm64/lib/liblvgl.a")
        target_link_libraries(ui_lvgl PUBLIC "${LVGL_PATH}/build-arm64/lib/liblvgl.a")
        message(STATUS "[CMake] UI 模块：链接 LVGL 静态库（build-arm64/ARM64 版）")
    elseif(EXISTS "${LVGL_PATH}/build/lib/liblvgl.a")
        target_link_libraries(ui_lvgl PUBLIC "${LVGL_PATH}/build/lib/liblvgl.a")
        message(STATUS "[CMake] UI 模块：链接 LVGL 静态库 liblvgl.a")
    elseif(EXISTS "${LVGL_PATH}/build/liblvgl.a")
        target_link_libraries(ui_lvgl PUBLIC "${LVGL_PATH}/build/liblvgl.a")
        message(STATUS "[CMake] UI 模块：链接 LVGL 静态库 liblvgl.a")
    else()
        message(WARNING
            "[CMake] LVGL_PATH 已设置但未找到 liblvgl.a\n"
            "  请先编译 LVGL：cd ${LVGL_PATH} && mkdir build && cd build && cmake .. && make")
    endif()
else()
    message(WARNING
        "[CMake] 未设置 LVGL_PATH 或路径下无 lvgl.h\n"
        "  请通过 -DLVGL_PATH=/path/to/lvgl 指定 LVGL 根目录\n"
        "  （ui_lvgl.h 包含 #include \"lvgl.h\"，缺少将导致编译失败）")
endif()


# -----------------------------------------------------------------------------
# 五、条件编译：NO_SDL 控制 SDL2 依赖
# -----------------------------------------------------------------------------
# NO_SDL=ON 时：不链接 SDL2，并定义 NO_SDL 宏让源码跳过 SDL2 初始化
# （ARM64 交叉编译 / 开发板 / Termux 场景）。
# NO_SDL=OFF 时：链接 SDL2（x86 桌面模式，通过根 CMakeLists 的检测结果）。
if(NO_SDL)
    # ARM64 模式：定义 NO_SDL 宏，源码中 #ifdef NO_SDL 跳过 SDL2 初始化
    # ui_main.c 中 NO_SDL 分支提供 sdl_init/process_events/deinit 的桩函数
    target_compile_definitions(ui_lvgl PUBLIC NO_SDL)
    message(STATUS "[CMake] UI 模块：NO_SDL=ON，跳过 SDL2 依赖（ARM64/开发板模式）")
else()
    # x86 模式：链接 SDL2（通过根 CMakeLists.txt 的 find_package(SDL2) 检测）
    if(SDL2_FOUND)
        # 添加 SDL2 头文件路径
        if(TARGET SDL2::SDL2)
            # 现代 CMake：用 IMPORTED target 自动传递 include 与 lib
            target_link_libraries(ui_lvgl PUBLIC SDL2::SDL2)
        else()
            # 兼容老式变量
            target_include_directories(ui_lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
            target_link_libraries(ui_lvgl PUBLIC ${SDL2_LIBRARIES} m)
        endif()
        message(STATUS "[CMake] UI 模块：启用 SDL2（x86 桌面模式）")
    else()
        message(WARNING
            "[CMake] 未找到 SDL2 开发库且 NO_SDL=OFF\n"
            "  请安装：sudo apt install libsdl2-dev\n"
            "  或设置 -DNO_SDL=ON 跳过 SDL2 依赖")
    endif()
endif()


# -----------------------------------------------------------------------------
# 六、测试程序
# -----------------------------------------------------------------------------
# 编译 test_*.c 为独立可执行文件。测试程序直接用源码方式编译各模块 .c 文件
# （不链接静态库），避免路径与链接顺序问题。
if(BUILD_TESTS)
    file(GLOB UI_LVGL_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${UI_LVGL_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})

        # 编译 ui_lvgl 模块源码进来
        target_sources(${test_name} PRIVATE ${UI_LVGL_SOURCES})

        # 编译 multi_proc.c 源码进来（避免依赖静态库路径）
        if(EXISTS "${CMAKE_SOURCE_DIR}/business/multi_proc/multi_proc.c")
            target_sources(${test_name} PRIVATE
                "${CMAKE_SOURCE_DIR}/business/multi_proc/multi_proc.c"
            )
        endif()

        # 编译 sqlite_db.c 源码进来（ui_chat_window.c 调用 chat_save/kb_add）
        if(EXISTS "${CMAKE_SOURCE_DIR}/business/sqlite_db/sqlite_db.c")
            target_sources(${test_name} PRIVATE
                "${CMAKE_SOURCE_DIR}/business/sqlite_db/sqlite_db.c"
            )
        endif()

        # 阶段 2：编译 hardware_sim.c 源码进来（ui_pipeline.c 调用 mem_sim_* 接口）
        if(EXISTS "${CMAKE_SOURCE_DIR}/business/hardware_sim/hardware_sim.c")
            target_sources(${test_name} PRIVATE
                "${CMAKE_SOURCE_DIR}/business/hardware_sim/hardware_sim.c"
            )
        endif()

        # 阶段 4：编译 rag.c 源码进来（ui_chat_window.c 调用 rag_retrieve）
        if(EXISTS "${CMAKE_SOURCE_DIR}/business/rag/rag.c")
            target_sources(${test_name} PRIVATE
                "${CMAKE_SOURCE_DIR}/business/rag/rag.c"
            )
        endif()

        # 头文件路径
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            "${CMAKE_SOURCE_DIR}/business/multi_proc"
            "${CMAKE_SOURCE_DIR}/business/sqlite_db"
            "${CMAKE_SOURCE_DIR}/business/hardware_sim"
            "${CMAKE_SOURCE_DIR}/business/rag"
        )

        # LVGL 头文件与库
        if(LVGL_PATH AND EXISTS "${LVGL_PATH}/lvgl.h")
            target_include_directories(${test_name} PRIVATE "${LVGL_PATH}" "${LVGL_PATH}/src")
            # 【阶段 6 修复：测试程序 LVGL 库查找顺序与主库一致】
            # 区分编译模式：交叉编译才用 build-arm64，x86 用 build/
            # （避免 x86 构建误链 ARM64 库导致 "EM: 183" 错误）。
            if(LVGL_LIBRARY)
                target_link_libraries(${test_name} PRIVATE "${LVGL_LIBRARY}")
            elseif(CMAKE_CROSSCOMPILING AND EXISTS "${LVGL_PATH}/build-arm64/lib/liblvgl.a")
                target_link_libraries(${test_name} PRIVATE "${LVGL_PATH}/build-arm64/lib/liblvgl.a")
            elseif(EXISTS "${LVGL_PATH}/build/lib/liblvgl.a")
                target_link_libraries(${test_name} PRIVATE "${LVGL_PATH}/build/lib/liblvgl.a")
            elseif(EXISTS "${LVGL_PATH}/build/liblvgl.a")
                target_link_libraries(${test_name} PRIVATE "${LVGL_PATH}/build/liblvgl.a")
            endif()
        endif()

        # SDL2 链接（NO_SDL=OFF 时）
        if(NOT NO_SDL AND SDL2_FOUND)
            if(TARGET SDL2::SDL2)
                target_link_libraries(${test_name} PRIVATE SDL2::SDL2)
            else()
                target_link_libraries(${test_name} PRIVATE ${SDL2_LIBRARIES} m)
            endif()
        endif()

        # pthread（ui_pipeline 用到）+ sqlite3（sqlite_db.c 依赖）
        if(TARGET SQLite::SQLite3)
            target_link_libraries(${test_name} PRIVATE pthread m SQLite::SQLite3)
        else()
            target_link_libraries(${test_name} PRIVATE pthread m sqlite3)
        endif()

        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()
