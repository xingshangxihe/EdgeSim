# =============================================================================
# EdgeSim 模块 CMakeLists.txt —— ai_engine/ocr_engine（OCR 引擎封装）
# =============================================================================
# 【模块定位】
#   AI 引擎层，封装 OCR 推理后端，将图片转为文字。
#   统一三接口：init / run / destroy
#
# 【支持的推理后端（按优先级自动适配）】
#   1. RKNN Lite   —— Rockchip NPU 后端（RV1106/RK3566 等，嵌入式专用）
#   2. ONNX Runtime —— 跨平台 CPU/GPU 后端（x86 桌面推荐）
#   3. OpenCV      —— 图像预处理辅助（读取/缩放/归一化，非独立推理后端）
#   4. Mock 桩     —— 默认模式，不依赖任何第三方库，仅测试接口连通性
#
# 【预留扩展位（当前仅声明，后续填入库路径即用）】
#   - RKNPU       —— 瑞芯微 RKNPU SDK（与 RKNN Lite 并行的另一套 API）
#   - TensorRT    —— NVIDIA GPU 推理后端
#
# 【条件编译说明（源码中的宏定义）】
#   HAS_RKNN_LITE     —— 启用 RKNN Lite 真实推理（对应 ocr_engine.c 的 #ifdef）
#   HAS_OPENCV        —— 启用 OpenCV 图像预处理（预留，源码后续补充 #ifdef 分支）
#   HAS_ONNXRUNTIME   —— 启用 ONNX Runtime 推理
#   HAS_RKNPU         —— 预留 RKNPU 后端宏
#   HAS_TENSORRT      —— 预留 TensorRT 后端宏
#   未定义任何宏时    —— 编译 mock 桩
#
# 【编译命令示例】
#   # 1. 默认 mock 桩模式（PC 开发测试）
#   mkdir build && cd build && cmake .. && make
#
#   # 2. 启用 OpenCV + ONNX Runtime（x86 桌面真实推理）
#   cmake -DWITH_OPENCV=ON -DWITH_ONNXRUNTIME=ON \
#         -DONNXRUNTIME_PATH=/path/to/onnxruntime ..
#
#   # 3. 启用 RKNN Lite（Rockchip 开发板）
#   cmake -DWITH_RKNN=ON -DRKNN_PATH=/path/to/rknn_sdk \
#         -DSTB_PATH=/path/to/stb ..
#
#   # 4. 预留 RKNPU / TensorRT（后续启用）
#   cmake -DWITH_RKNPU=ON -DRKNPU_PATH=/path/to/rknpu ..
#   cmake -DWITH_TENSORRT=ON -DTENSORRT_PATH=/path/to/tensorrt ..
# =============================================================================


# -----------------------------------------------------------------------------
# 一、收集源文件（排除 test_*.c）
# -----------------------------------------------------------------------------
file(GLOB OCR_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(FILTER OCR_ENGINE_SOURCES EXCLUDE REGEX ".*/test_.*\\.c$")


# -----------------------------------------------------------------------------
# 二、创建静态库（libocr_engine.a）
# -----------------------------------------------------------------------------
# 重要：当启用 ONNX Runtime 时，ocr_engine.c 包含 <opencv2/opencv.hpp>（C++ 头文件），
# 使用了 cv::Mat / std::vector 等 C++ 语法，必须用 C++ 编译器编译。
# set_source_files_properties 让 CMake 对该 .c 文件使用 CXX 编译器（g++）。
if(EDGE_USE_ONNXRUNTIME)
    set_source_files_properties(${OCR_ENGINE_SOURCES} PROPERTIES LANGUAGE CXX)
endif()

if(OCR_ENGINE_SOURCES)
    add_library(ocr_engine STATIC ${OCR_ENGINE_SOURCES})
else()
    add_library(ocr_engine STATIC)
endif()

# 头文件路径对外暴露
target_include_directories(ocr_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# =============================================================================
# 三、推理后端配置（核心：多后端自动适配）
# =============================================================================
# 按优先级依次检测各后端，找到则定义对应宏并链接库。
# 多后端可共存（如 RKNN + OpenCV：RKNN 做推理，OpenCV 做预处理）。

# -----------------------------------------------------------------------------
# 3.1 RKNN Lite 后端（Rockchip NPU）
# -----------------------------------------------------------------------------
# WITH_RKNN=ON 且 RKNN_PATH 非空时启用 RKNN Lite 推理：
#   - 定义 HAS_RKNN_LITE 宏
#   - 添加 rknn_api.h 头文件路径
#   - 可选添加 stb_image 头文件路径（图片解码用）
#   - 链接 librknnrt 运行时库（静态优先，动态库兜底）
if(WITH_RKNN AND RKNN_PATH)
    if(EXISTS "${RKNN_PATH}/include/rknn_api.h")
        # 定义 HAS_RKNN_LITE 宏（ocr_engine.c 中 #ifdef HAS_RKNN_LITE 切换）
        target_compile_definitions(ocr_engine PUBLIC HAS_RKNN_LITE)

        # 添加 RKNN 头文件路径
        target_include_directories(ocr_engine PUBLIC "${RKNN_PATH}/include")

        # stb_image 头文件路径（独立指定，可与 RKNN_PATH 不同）
        # stb_image 是单头文件库，用于图片读取（RKNN Lite 自身不带图片解码）
        if(STB_PATH AND EXISTS "${STB_PATH}/stb_image.h")
            target_include_directories(ocr_engine PUBLIC "${STB_PATH}")
            message(STATUS "[CMake] OCR 引擎：stb_image 路径: ${STB_PATH}")
        elseif(STB_PATH)
            message(WARNING "[CMake] STB_PATH 已设置但未找到 stb_image.h: ${STB_PATH}")
        endif()

        # 链接 RKNN 运行时库
        # 优先链接静态库（嵌入式推荐），找不到则用动态库
        if(EXISTS "${RKNN_PATH}/lib/librknnrt.a")
            target_link_libraries(ocr_engine PUBLIC
                "${RKNN_PATH}/lib/librknnrt.a"
                pthread m
            )
            message(STATUS "[CMake] OCR 引擎：链接 RKNN 静态库 librknnrt.a")
        elseif(EXISTS "${RKNN_PATH}/lib/librknnrt.so")
            target_link_directories(ocr_engine PUBLIC "${RKNN_PATH}/lib")
            target_link_libraries(ocr_engine PUBLIC rknnrt pthread m)
            message(STATUS "[CMake] OCR 引擎：链接 RKNN 动态库 librknnrt.so")
        else()
            message(WARNING
                "[CMake] RKNN_PATH 已设置但未找到 librknnrt.a/.so\n"
                "  请检查 ${RKNN_PATH}/lib/ 目录")
        endif()

        message(STATUS "[CMake] OCR 引擎：启用 RKNN Lite 后端，路径: ${RKNN_PATH}")
    else()
        message(WARNING
            "[CMake] RKNN_PATH 已设置但未找到 ${RKNN_PATH}/include/rknn_api.h\n"
            "  请检查路径是否正确，回退到 mock 桩模式")
    endif()
endif()


# -----------------------------------------------------------------------------
# 3.2 OpenCV 后端（新增：图像预处理）
# -----------------------------------------------------------------------------
# OpenCV 用于图片读取、缩放、归一化等预处理操作。
# 可与 RKNN Lite 或 ONNX Runtime 组合使用。
# 检测结果由根 CMakeLists.txt 的 find_package(OpenCV) 完成，存于 EDGE_USE_OPENCV。
if(EDGE_USE_OPENCV)
    # 定义 HAS_OPENCV 宏（源码后续补充 #ifdef HAS_OPENCV 分支后生效）
    target_compile_definitions(ocr_engine PUBLIC HAS_OPENCV)

    # 添加 OpenCV 头文件路径
    if(TARGET OpenCV::opencv_core)
        # 现代 CMake 用 IMPORTED target 链接（推荐方式）
        target_link_libraries(ocr_engine PUBLIC
            OpenCV::opencv_core
            OpenCV::opencv_imgproc
            OpenCV::opencv_imgcodecs
        )
    else()
        # 兼容老式变量
        target_include_directories(ocr_engine PUBLIC ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(ocr_engine PUBLIC ${OpenCV_LIBS})
    endif()

    message(STATUS "[CMake] OCR 引擎：启用 OpenCV 图像预处理（版本: ${OpenCV_VERSION}）")
endif()


# -----------------------------------------------------------------------------
# 3.3 ONNX Runtime 后端（新增：跨平台推理）
# -----------------------------------------------------------------------------
# ONNX Runtime 是微软开源的跨平台推理引擎，支持 CPU/GPU/NPU。
# 用于在 x86 桌面或非 Rockchip 平台上运行 ONNX 格式的 OCR 模型。
# 检测结果由根 CMakeLists.txt 完成，存于 EDGE_USE_ONNXRUNTIME。
if(EDGE_USE_ONNXRUNTIME)
    # 定义 HAS_ONNXRUNTIME 宏（源码后续补充 #ifdef HAS_ONNXRUNTIME 分支后生效）
    target_compile_definitions(ocr_engine PUBLIC HAS_ONNXRUNTIME)

    # 添加 ONNX Runtime 头文件路径
    # 头文件结构：${ONNXRUNTIME_PATH}/include/onnxruntime_c_api.h
    target_include_directories(ocr_engine PUBLIC "${ONNXRUNTIME_PATH}/include")

    # 链接 ONNX Runtime 库
    # 库文件结构：
    #   Linux  : ${ONNXRUNTIME_PATH}/lib/libonnxruntime.so
    #   Windows: ${ONNXRUNTIME_PATH}/lib/onnxruntime.lib
    #   静态库 : ${ONNXRUNTIME_PATH}/lib/libonnxruntime.a（部分版本提供）
    if(EXISTS "${ONNXRUNTIME_PATH}/lib/libonnxruntime.a")
        # 静态库优先（嵌入式推荐，避免运行时依赖 .so）
        target_link_libraries(ocr_engine PUBLIC
            "${ONNXRUNTIME_PATH}/lib/libonnxruntime.a"
            pthread dl m
        )
        message(STATUS "[CMake] OCR 引擎：链接 ONNX Runtime 静态库")
    elseif(EXISTS "${ONNXRUNTIME_PATH}/lib/libonnxruntime.so")
        # 动态库（桌面推荐，便于升级）
        target_link_directories(ocr_engine PUBLIC "${ONNXRUNTIME_PATH}/lib")
        target_link_libraries(ocr_engine PUBLIC onnxruntime pthread dl m)
        message(STATUS "[CMake] OCR 引擎：链接 ONNX Runtime 动态库")
        # 运行时需设置 LD_LIBRARY_PATH 指向 ${ONNXRUNTIME_PATH}/lib
    else()
        message(WARNING
            "[CMake] ONNXRUNTIME_PATH 已设置但未找到 onnxruntime 库\n"
            "  请检查 ${ONNXRUNTIME_PATH}/lib/ 目录")
    endif()

    message(STATUS "[CMake] OCR 引擎：启用 ONNX Runtime 推理后端，路径: ${ONNXRUNTIME_PATH}")
endif()


# -----------------------------------------------------------------------------
# 3.4 预留扩展位：RKNPU 后端
# -----------------------------------------------------------------------------
# RKNPU 是瑞芯微另一套 NPU SDK API（与 RKNN Lite 并行）。
# 当前仅声明选项与路径变量，后续填入库路径即可启用。
# 启用方式：cmake -DWITH_RKNPU=ON -DRKNPU_PATH=/path/to/rknpu ..
if(WITH_RKNPU)
    # RKNPU_PATH 变量（预留，用户后续设置）
    set(RKNPU_PATH "" CACHE PATH "RKNPU SDK 根目录（含 include/ 与 lib/）")

    if(RKNPU_PATH AND EXISTS "${RKNPU_PATH}/include")
        # 定义 HAS_RKNPU 宏（源码后续补充 #ifdef HAS_RKNPU 分支后生效）
        target_compile_definitions(ocr_engine PUBLIC HAS_RKNPU)
        target_include_directories(ocr_engine PUBLIC "${RKNPU_PATH}/include")

        # 预留链接位（用户后续填入实际库文件名）
        # 常见库：librknn_api.a / librknnrt.so / librknn_kernel.so
        if(EXISTS "${RKNPU_PATH}/lib")
            target_link_directories(ocr_engine PUBLIC "${RKNPU_PATH}/lib")
            # target_link_libraries(ocr_engine PUBLIC rknn_api)  # 取消注释启用
        endif()

        message(STATUS "[CMake] OCR 引擎：启用 RKNPU 后端（预留），路径: ${RKNPU_PATH}")
    else()
        message(STATUS "[CMake] OCR 引擎：RKNPU 已声明但路径未设置（预留扩展位）")
    endif()
endif()


# -----------------------------------------------------------------------------
# 3.5 预留扩展位：TensorRT 后端
# -----------------------------------------------------------------------------
# TensorRT 是 NVIDIA 的高性能推理引擎，用于 GPU 加速。
# 当前仅声明选项与路径变量，后续填入库路径即可启用。
# 启用方式：cmake -DWITH_TENSORRT=ON -DTENSORRT_PATH=/path/to/tensorrt ..
if(WITH_TENSORRT)
    # TENSORRT_PATH 变量（预留，用户后续设置）
    set(TENSORRT_PATH "" CACHE PATH "TensorRT 安装目录（含 include/ 与 lib/）")

    if(TENSORRT_PATH AND EXISTS "${TENSORRT_PATH}/include")
        # 定义 HAS_TENSORRT 宏（源码后续补充 #ifdef HAS_TENSORRT 分支后生效）
        target_compile_definitions(ocr_engine PUBLIC HAS_TENSORRT)
        target_include_directories(ocr_engine PUBLIC "${TENSORRT_PATH}/include")

        # 预留链接位（用户后续填入实际库文件名）
        # 常见库：libnvinfer.so / libnvinfer_plugin.so / libnvonnxparser.so
        if(EXISTS "${TENSORRT_PATH}/lib")
            target_link_directories(ocr_engine PUBLIC "${TENSORRT_PATH}/lib")
            # target_link_libraries(ocr_engine PUBLIC nvinfer nvonnxparser)  # 取消注释启用
        endif()

        message(STATUS "[CMake] OCR 引擎：启用 TensorRT 后端（预留），路径: ${TENSORRT_PATH}")
    else()
        message(STATUS "[CMake] OCR 引擎：TensorRT 已声明但路径未设置（预留扩展位）")
    endif()
endif()


# -----------------------------------------------------------------------------
# 3.6 无任何后端启用时的提示（mock 桩模式）
# -----------------------------------------------------------------------------
# 检查是否启用了任何真实后端
set(OCR_HAS_REAL_BACKEND OFF)
if((WITH_RKNN AND RKNN_PATH AND EXISTS "${RKNN_PATH}/include/rknn_api.h")
   OR EDGE_USE_OPENCV
   OR EDGE_USE_ONNXRUNTIME
   OR (WITH_RKNPU AND RKNPU_PATH)
   OR (WITH_TENSORRT AND TENSORRT_PATH))
    set(OCR_HAS_REAL_BACKEND ON)
endif()

if(NOT OCR_HAS_REAL_BACKEND)
    message(STATUS "[CMake] OCR 引擎：mock 桩模式（未启用任何真实后端）")
    message(STATUS "  可用选项：-DWITH_RKNN=ON / -DWITH_OPENCV=ON / -DWITH_ONNXRUNTIME=ON")
endif()


# -----------------------------------------------------------------------------
# 四、测试程序
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    file(GLOB OCR_ENGINE_TESTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c"
    )
    foreach(test_src ${OCR_ENGINE_TESTS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        # 测试程序用源码方式编译进来
        target_sources(${test_name} PRIVATE ${OCR_ENGINE_SOURCES})
        # ONNX Runtime 模式下 ocr_engine.c 含 C++ 语法，测试程序也需用 C++ 编译
        if(EDGE_USE_ONNXRUNTIME)
            set_source_files_properties(${test_src} PROPERTIES LANGUAGE CXX)
            set_source_files_properties(${OCR_ENGINE_SOURCES} PROPERTIES LANGUAGE CXX)
        endif()
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        # 继承 ocr_engine 的所有后端配置（宏定义、头文件路径、链接库）
        # 用 PRIVATE 链接 ocr_engine 静态库会导致 main 符号冲突（test 自带 main），
        # 所以这里用源码方式编译，并手动传递后端配置
        target_compile_definitions(${test_name} PRIVATE
            $<TARGET_PROPERTY:ocr_engine,INTERFACE_COMPILE_DEFINITIONS>
        )
        target_include_directories(${test_name} PRIVATE
            $<TARGET_PROPERTY:ocr_engine,INTERFACE_INCLUDE_DIRECTORIES>
        )

        # 链接后端库（与 ocr_engine 一致）
        if(WITH_RKNN AND RKNN_PATH AND EXISTS "${RKNN_PATH}/include/rknn_api.h")
            if(EXISTS "${RKNN_PATH}/lib/librknnrt.a")
                target_link_libraries(${test_name} PRIVATE "${RKNN_PATH}/lib/librknnrt.a" pthread m)
            elseif(EXISTS "${RKNN_PATH}/lib/librknnrt.so")
                target_link_libraries(${test_name} PRIVATE rknnrt pthread m)
            endif()
        endif()
        if(EDGE_USE_OPENCV)
            if(TARGET OpenCV::opencv_core)
                target_link_libraries(${test_name} PRIVATE
                    OpenCV::opencv_core OpenCV::opencv_imgproc OpenCV::opencv_imgcodecs)
            else()
                target_link_libraries(${test_name} PRIVATE ${OpenCV_LIBS})
            endif()
        endif()
        if(EDGE_USE_ONNXRUNTIME)
            if(EXISTS "${ONNXRUNTIME_PATH}/lib/libonnxruntime.a")
                target_link_libraries(${test_name} PRIVATE
                    "${ONNXRUNTIME_PATH}/lib/libonnxruntime.a" pthread dl m)
            elseif(EXISTS "${ONNXRUNTIME_PATH}/lib/libonnxruntime.so")
                target_link_libraries(${test_name} PRIVATE onnxruntime pthread dl m)
            endif()
        endif()

        message(STATUS "[CMake] 测试程序: ${test_name}")
    endforeach()
endif()
