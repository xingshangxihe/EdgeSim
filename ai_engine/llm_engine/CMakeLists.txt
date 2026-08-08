# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— ai_engine/llm_engine（LLM 引擎封装）
# =============================================================================
# 【模块定位】
#   AI 引擎层，封装 llama.cpp，加载 INT8 量化的 Qwen1.8B 模型，提供离线对话推理。
#   统一三接口：init / run / destroy
#
# 【条件编译说明】
#   默认编译 mock 桩（不依赖 llama.cpp，仅测试接口连通性）
#   启用真实推理：
#     cmake -DWITH_LLAMA=ON -DLLAMA_PATH=/path/to/llama.cpp ..
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
file(GLOB LLM_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER LLM_ENGINE_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libllm_engine.a）
# -----------------------------------------------------------------------------
if(LLM_ENGINE_SOURCES)
    add_library(llm_engine STATIC ${LLM_ENGINE_SOURCES})
else()
    add_library(llm_engine STATIC)
endif()

# 头文件路径对外暴露
target_include_directories(llm_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# -----------------------------------------------------------------------------
# 三、条件编译：WITH_LLAMA + LLAMA_PATH
# -----------------------------------------------------------------------------
# 检测 WITH_LLAMA=ON 且 LLAMA_PATH 非空时启用真实 llama.cpp：
#   - 定义 HAS_LLAMA_CPP 宏（源码中 #ifdef 切换真实/mock）
#   - 添加 llama.h / ggml.h 头文件路径
#   - 链接 libllama.a + libggml.a（含 pthread / dl / m 依赖）
# 否则编译 mock 桩（不依赖第三方库）。
if(WITH_LLAMA AND LLAMA_PATH)
    # 校验路径是否包含 llama.h 头文件
    if(EXISTS "${LLAMA_PATH}/include/llama.h")
        # 定义 HAS_LLAMA_CPP 宏（源码中 #ifdef HAS_LLAMA_CPP 切换真实/mock）
        target_compile_definitions(llm_engine PUBLIC HAS_LLAMA_CPP)
        # 添加头文件路径（llama.h 与 ggml.h）
        target_include_directories(llm_engine PUBLIC
            "${LLAMA_PATH}/include"
            "${LLAMA_PATH}/ggml/include"
        )
        # 链接 llama.cpp 库
        # 新版 llama.cpp (2025+) 默认构建共享库 (.so) 到 build/bin/
        # 旧版 llama.cpp 构建静态库 (.a) 到 build/
        # 这里两种都支持，优先静态库（运行时不需要 LD_LIBRARY_PATH）
        if(EXISTS "${LLAMA_PATH}/build/libllama.a")
            # 旧版：静态库
            target_link_libraries(llm_engine PUBLIC
                "${LLAMA_PATH}/build/libllama.a"
                "${LLAMA_PATH}/build/libggml.a"
                pthread dl m
            )
            message(STATUS "[CMake] LLM 引擎：启用真实 llama.cpp (静态库)，路径: ${LLAMA_PATH}")
        elseif(EXISTS "${LLAMA_PATH}/build/bin/libllama.so")
            # 新版：共享库
            target_link_libraries(llm_engine PUBLIC
                "${LLAMA_PATH}/build/bin/libllama.so"
                "${LLAMA_PATH}/build/bin/libggml.so"
                "${LLAMA_PATH}/build/bin/libggml-base.so"
                "${LLAMA_PATH}/build/bin/libggml-cpu.so"
                "${LLAMA_PATH}/build/bin/libllama-common.so"
                pthread dl m
            )
            # 设置 RPATH，让运行时能找到共享库
            set_target_properties(llm_engine PROPERTIES
                BUILD_RPATH "${LLAMA_PATH}/build/bin"
                INSTALL_RPATH "${LLAMA_PATH}/build/bin"
            )
            message(STATUS "[CMake] LLM 引擎：启用真实 llama.cpp (共享库)，路径: ${LLAMA_PATH}")
            message(STATUS "[CMake] 运行时需设置: export LD_LIBRARY_PATH=${LLAMA_PATH}/build/bin:$LD_LIBRARY_PATH")
        else()
            message(WARNING
                "[CMake] LLAMA_PATH 已设置但未找到 libllama.a 或 libllama.so\n"
                "  请先编译 llama.cpp：cd ${LLAMA_PATH} && mkdir build && cd build && cmake .. && make")
        endif()
    else()
        message(WARNING
            "[CMake] LLAMA_PATH 已设置但未找到 ${LLAMA_PATH}/include/llama.h\n"
            "  请检查路径是否正确，回退到 mock 桩模式")
    endif()
else()
    message(STATUS "[CMake] LLM 引擎：mock 桩模式（未设置 WITH_LLAMA=ON + LLAMA_PATH）")
endif()


# -----------------------------------------------------------------------------
# 四、测试程序
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    file(GLOB LLM_ENGINE_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${LLM_ENGINE_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        # 测试程序用源码方式编译进来
        target_sources(${test_name} PRIVATE ${LLM_ENGINE_SOURCES})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        # 若启用了真实 llama.cpp，测试程序也需要链接对应库
        if(WITH_LLAMA AND LLAMA_PATH AND EXISTS "${LLAMA_PATH}/include/llama.h")
            target_compile_definitions(${test_name} PRIVATE HAS_LLAMA_CPP)
            target_include_directories(${test_name} PRIVATE
                "${LLAMA_PATH}/include"
                "${LLAMA_PATH}/ggml/include"
            )
            if(EXISTS "${LLAMA_PATH}/build/libllama.a")
                target_link_libraries(${test_name} PRIVATE
                    "${LLAMA_PATH}/build/libllama.a"
                    "${LLAMA_PATH}/build/libggml.a"
                    pthread dl m
                )
            elseif(EXISTS "${LLAMA_PATH}/build/bin/libllama.so")
                target_link_libraries(${test_name} PRIVATE
                    "${LLAMA_PATH}/build/bin/libllama.so"
                    "${LLAMA_PATH}/build/bin/libggml.so"
                    "${LLAMA_PATH}/build/bin/libggml-base.so"
                    "${LLAMA_PATH}/build/bin/libggml-cpu.so"
                    "${LLAMA_PATH}/build/bin/libllama-common.so"
                    pthread dl m
                )
            endif()
        endif()
        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()
