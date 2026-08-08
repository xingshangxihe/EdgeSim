# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— ai_engine/asr_engine（ASR 语音识别引擎封装）
# =============================================================================
# 【模块定位】
#   AI 引擎层，封装 whisper.cpp，加载 INT8 量化的 Whisper 模型，
#   将音频文件（麦克风录音）转写为文字。
#   统一三接口：init / run / destroy
#
# 【条件编译说明】
#   默认编译 mock 桩（不依赖 whisper.cpp，仅测试接口连通性）
#   启用真实推理：
#     cmake -DWITH_WHISPER=ON -DWHISPER_PATH=/path/to/whisper.cpp ..
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
file(GLOB ASR_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER ASR_ENGINE_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libasr_engine.a）
# -----------------------------------------------------------------------------
if(ASR_ENGINE_SOURCES)
    add_library(asr_engine STATIC ${ASR_ENGINE_SOURCES})
else()
    add_library(asr_engine STATIC)
endif()

# 头文件路径对外暴露
target_include_directories(asr_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# -----------------------------------------------------------------------------
# 三、条件编译：WITH_WHISPER + WHISPER_PATH
# -----------------------------------------------------------------------------
# 检测 WITH_WHISPER=ON 且 WHISPER_PATH 非空时启用真实 whisper.cpp：
#   - 定义 HAS_WHISPER_CPP 宏（源码中 #ifdef 切换真实/mock）
#   - 添加 whisper.h / ggml.h 头文件路径
#   - 链接 libwhisper.a + libggml.a（含 pthread / dl / m 依赖）
# 否则编译 mock 桩（不依赖第三方库）。
if(WITH_WHISPER AND WHISPER_PATH)
    if(EXISTS "${WHISPER_PATH}/include/whisper.h")
        # 定义 HAS_WHISPER_CPP 宏
        target_compile_definitions(asr_engine PUBLIC HAS_WHISPER_CPP)
        # 添加头文件路径
        target_include_directories(asr_engine PUBLIC
            "${WHISPER_PATH}/include"
            "${WHISPER_PATH}/ggml/include"
        )
        # 新版 whisper.cpp 编译为共享库（.so），在 build/bin/ 下
        # 旧版为静态库（.a），在 build/ 下
        if(EXISTS "${WHISPER_PATH}/build/bin/libwhisper.so")
            target_link_libraries(asr_engine PUBLIC
                "${WHISPER_PATH}/build/bin/libwhisper.so"
                "${WHISPER_PATH}/build/bin/libggml.so"
                pthread dl m
            )
            message(STATUS "[CMake] ASR 引擎：启用真实 whisper.cpp（共享库），路径: ${WHISPER_PATH}")
        elseif(EXISTS "${WHISPER_PATH}/build/libwhisper.a")
            target_link_libraries(asr_engine PUBLIC
                "${WHISPER_PATH}/build/libwhisper.a"
                "${WHISPER_PATH}/build/libggml.a"
                pthread dl m
            )
            message(STATUS "[CMake] ASR 引擎：启用真实 whisper.cpp（静态库），路径: ${WHISPER_PATH}")
        else()
            message(WARNING
                "[CMake] WHISPER_PATH 已设置但未找到 libwhisper.so 或 libwhisper.a\n"
                "  请先编译 whisper.cpp：cd ${WHISPER_PATH} && mkdir build && cd build && cmake .. && make")
        endif()
    else()
        message(WARNING
            "[CMake] WHISPER_PATH 已设置但未找到 ${WHISPER_PATH}/include/whisper.h\n"
            "  请检查路径是否正确，回退到 mock 桩模式")
    endif()
else()
    message(STATUS "[CMake] ASR 引擎：mock 桩模式（未设置 WITH_WHISPER=ON + WHISPER_PATH）")
endif()


# -----------------------------------------------------------------------------
# 四、测试程序
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    file(GLOB ASR_ENGINE_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${ASR_ENGINE_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_sources(${test_name} PRIVATE ${ASR_ENGINE_SOURCES})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        if(WITH_WHISPER AND WHISPER_PATH AND EXISTS "${WHISPER_PATH}/build/libwhisper.a")
            target_compile_definitions(${test_name} PRIVATE HAS_WHISPER_CPP)
            target_include_directories(${test_name} PRIVATE
                "${WHISPER_PATH}/include"
                "${WHISPER_PATH}/ggml/include"
            )
            target_link_libraries(${test_name} PRIVATE
                "${WHISPER_PATH}/build/libwhisper.a"
                "${WHISPER_PATH}/build/libggml.a"
                pthread dl m
            )
        endif()
        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()
