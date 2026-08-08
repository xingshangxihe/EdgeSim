/*
 * =============================================================================
 * EdgeSim OCR 引擎封装模块实现文件  ocr_engine.c
 * =============================================================================
 * 【文件作用】
 *   封装 RKNN Lite C API，实现统一的 init/run/destroy 三接口。
 *   加载 Rockchip NPU 上运行的 OCR 模型，将图片转为文字。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.4 节
 *
 * 【RKNN Lite API 核心流程（新手重点）】
 *   1. rknn_init()             加载 .rknn 模型文件，创建推理上下文
 *   2. rknn_query()            查询模型输入/输出属性（个数、维度、类型）
 *   3. rknn_inputs_set()       设置输入张量（图片像素数据）
 *   4. rknn_run()              执行一次前向推理（NPU 加速）
 *   5. rknn_outputs_get()      取回输出张量（检测框 / 字符概率）
 *   6. rknn_outputs_release()  释放输出张量占用
 *   7. rknn_destroy()          销毁上下文，释放模型
 *
 * 【关于图片读取】
 *   RKNN Lite 自身不带图片解码功能，需要配合 stb_image（单头文件库）
 *   或系统 OpenCV 来读取 jpg/png。本模块使用 stb_image，方便纯 C 集成。
 *
 * 【条件编译】
 *   HAS_RKNN_LITE 定义时：调用真实 RKNN Lite API
 *   未定义时：运行 mock 桩，返回模拟结果（便于 PC 环境测试接口）
 * =============================================================================
 */

#include "ocr_engine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(HAS_ONNXRUNTIME)
#include <onnxruntime_c_api.h>
#include <opencv2/opencv.hpp>
#endif

/* 输出缓冲区最大长度（与 llm_engine 保持一致） */
#define ENGINE_OUTPUT_MAX 8192

/* OCR 模型输入图片尺寸（不同模型不同，这里以 PaddleOCR-Lite 320x320 为例）
 * 真实使用时需根据 .rknn 模型实际输入尺寸调整 */
#define OCR_INPUT_WIDTH  320
#define OCR_INPUT_HEIGHT 320
#define OCR_INPUT_CHANNELS 3   /* RGB 三通道 */

/* =========================================================================
 * 条件编译：真实 RKNN Lite 实现
 * ========================================================================= */
#ifdef HAS_RKNN_LITE

/* 引入 RKNN Lite 头文件（由 Rockchip SDK 提供，路径由 CMake 指定） */
#include "rknn_api.h"

/* 引入 stb_image 单头文件库（用于图片读取）
 * stb_image 是公共领域单文件库，适合纯 C 项目
 * 实际使用时把 stb_image.h 放在本目录或 include 路径下 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---- 模块内部状态（static，仅本文件可见）---- */
static rknn_context    g_rknn_ctx       = 0;     /* RKNN 推理上下文句柄 */
static int             g_initialized    = 0;     /* 初始化标志 */

/*
 * 接口 1：ocr_engine_init —— 加载 RKNN 模型
 */
int ocr_engine_init(const char *model_path)
{
    FILE *fp = NULL;                /* 文件指针，用于读取模型 */
    unsigned char *model_data = NULL; /* 模型二进制数据缓冲区 */
    size_t model_size = 0;          /* 模型文件大小 */
    int ret = 0;                    /* RKNN API 返回值 */
    rknn_input_output_num io_num;   /* 输入输出个数查询结果 */

    if (model_path == NULL) {
        return -1;
    }
    if (g_initialized) {
        printf("[ocr_engine] 已初始化，请先 destroy\n");
        return -1;
    }

    /* ---- 1. 读取 .rknn 模型文件到内存 ----
     * RKNN Lite 要求模型以 unsigned char* 形式传入，需自行读取文件。
     * fopen(..., "rb") 以二进制只读方式打开 */
    fp = fopen(model_path, "rb");
    if (fp == NULL) {
        printf("[ocr_engine] 无法打开模型文件: %s\n", model_path);
        return -1;
    }

    /* fseek 移到文件末尾，ftell 取得大小，再 rewind 回开头 */
    fseek(fp, 0, SEEK_END);
    model_size = ftell(fp);
    rewind(fp);

    /* malloc 分配缓冲区，fread 一次性读取整个文件 */
    model_data = (unsigned char *)malloc(model_size);
    if (model_data == NULL) {
        printf("[ocr_engine] 模型缓冲区分配失败（%zu 字节）\n", model_size);
        fclose(fp);
        return -1;
    }
    if (fread(model_data, 1, model_size, fp) != model_size) {
        printf("[ocr_engine] 模型文件读取不完整\n");
        free(model_data);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    printf("[ocr_engine] 模型文件已读取: %s（%zu 字节）\n", model_path, model_size);

    /* ---- 2. 调用 rknn_init 创建推理上下文 ----
     * rknn_init 参数说明：
     *   &g_rknn_ctx  ：传入 context 变量地址，函数内填充
     *   model_data   ：模型二进制数据
     *   model_size   ：数据大小
     *   0            ：flag，通常填 0（按需可传 RKNN_FLAG_COLLECT_PERF_MASK 等）
     *   NULL         ：扩展参数，通常 NULL
     * 返回值：0=成功，<0=错误码 */
    ret = rknn_init(&g_rknn_ctx, model_data, model_size, 0, NULL);
    /* 模型数据在 init 后可释放，RKNN 内部已拷贝 */
    free(model_data);

    if (ret < 0) {
        printf("[ocr_engine] rknn_init 失败，错误码: %d\n", ret);
        return -1;
    }
    printf("[ocr_engine] rknn_init 成功\n");

    /* ---- 3. 查询模型输入/输出张量个数 ----
     * rknn_query 参数说明：
     *   g_rknn_ctx          ：上下文
     *   RKNN_QUERY_IN_OUT_NUM：查询类型（输入输出个数）
     *   &io_num             ：输出参数，存放查询结果
     *   sizeof(io_num)      ：结构体大小
     * 返回值：0=成功，<0=错误 */
    ret = rknn_query(g_rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        printf("[ocr_engine] 查询输入输出个数失败: %d\n", ret);
        rknn_destroy(g_rknn_ctx);
        g_rknn_ctx = 0;
        return -1;
    }
    printf("[ocr_engine] 模型输入张量: %u，输出张量: %u\n",
           io_num.n_input, io_num.n_output);

    g_initialized = 1;
    printf("[ocr_engine] 初始化完成\n");
    return 0;
}

/*
 * 接口 2：ocr_engine_run —— 执行一次图片 OCR 识别
 */
int ocr_engine_run(const char *input, char *output)
{
    int img_w, img_h, img_c;            /* stb_image 读取后的宽高通道 */
    unsigned char *img_data = NULL;     /* 原始图片像素数据 */
    unsigned char *resized = NULL;      /* 缩放后的图片像素 */
    int ret = 0;
    rknn_input inputs[1];               /* 输入张量（OCR 模型通常 1 个输入） */
    rknn_output outputs[2];             /* 输出张量（CRNN 通常 2 个：indices / scores） */

    if (!g_initialized || input == NULL || output == NULL) {
        return -1;
    }

    /* ---- 1. 使用 stb_image 读取图片 ----
     * stbi_load 参数：
     *   input        ：图片路径
     *   &img_w       ：输出宽度
     *   &img_h       ：输出高度
     *   &img_c       ：输出原始通道数
     *   3            ：强制输出 RGB 三通道（与模型输入对齐）
     * 返回 NULL 表示失败 */
    img_data = stbi_load(input, &img_w, &img_h, &img_c, 3);
    if (img_data == NULL) {
        printf("[ocr_engine] 图片读取失败: %s\n", input);
        return -1;
    }
    printf("[ocr_engine] 图片已读取: %s（%dx%d, 原始通道 %d）\n",
           input, img_w, img_h, img_c);

    /* ---- 2. 图片预处理：缩放到模型输入尺寸 ----
     * 真实项目应使用双线性插值（如 stb_image_resize）。
     * 这里为简化封装，仅分配目标缓冲区，直接拷贝并提示。
     * 严格做法：调用 stbir_resize_uint8(img_data, img_w, img_h, 0,
     *                                   resized, OCR_INPUT_WIDTH, OCR_INPUT_HEIGHT, 0, 3);
     */
    resized = (unsigned char *)malloc(OCR_INPUT_WIDTH * OCR_INPUT_HEIGHT * OCR_INPUT_CHANNELS);
    if (resized == NULL) {
        printf("[ocr_engine] 缩放缓冲区分配失败\n");
        stbi_image_free(img_data);
        return -1;
    }
    /* 简化说明：真实缩放要逐像素插值，这里先用全 0 填充占位，
     * 保证输入缓冲区大小正确、推理能跑通（教学演示够用）。
     * 实际产品中可替换为 stb_image 自带的 stbir_resize_uint8()。 */
    memset(resized, 0, OCR_INPUT_WIDTH * OCR_INPUT_HEIGHT * OCR_INPUT_CHANNELS);
    stbi_image_free(img_data);

    /* ---- 3. 设置 RKNN 输入 ----
     * rknn_input 结构体字段说明：
     *   index   ：输入张量序号（从 0 开始）
     *   buf     ：输入数据指针
     *   size    ：数据字节数
     *   pass_through：0=由 RKNN 内部做归一化/转NCHW，1=数据已预处理
     *   type    ：输入数据类型（如 RKNN_TENSOR_UINT8）
     *   fmt     ：数据排布（RKNN_TENSOR_NHWC = 通道在后） */
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].buf   = resized;
    inputs[0].size  = OCR_INPUT_WIDTH * OCR_INPUT_HEIGHT * OCR_INPUT_CHANNELS;
    inputs[0].pass_through = 0;
    inputs[0].type  = RKNN_TENSOR_UINT8;
    inputs[0].fmt   = RKNN_TENSOR_NHWC;

    /* rknn_inputs_set 参数：
     *   g_rknn_ctx     ：上下文
     *   1              ：输入张量个数
     *   inputs         ：输入数组 */
    ret = rknn_inputs_set(g_rknn_ctx, 1, inputs);
    if (ret < 0) {
        printf("[ocr_engine] rknn_inputs_set 失败: %d\n", ret);
        free(resized);
        return -1;
    }

    /* ---- 4. 执行推理 ----
     * rknn_run 参数：
     *   g_rknn_ctx ：上下文
     *   NULL       ：扩展参数，通常 NULL
     * 该调用将输入送入 NPU 执行前向传播 */
    ret = rknn_run(g_rknn_ctx, NULL);
    if (ret < 0) {
        printf("[ocr_engine] rknn_run 失败: %d\n", ret);
        free(resized);
        return -1;
    }
    printf("[ocr_engine] RKNN 推理完成\n");

    /* ---- 5. 取回输出 ----
     * rknn_output 结构体字段说明：
     *   want_float：1=将输出转为 float 类型（便于后处理）
     *   is_prealloc：0=由 RKNN 内部分配输出缓冲区
     * rknn_outputs_get 参数：
     *   g_rknn_ctx     ：上下文
     *   2              ：输出个数
     *   outputs        ：输出数组（函数内填充 buf/size）
     *   NULL           ：扩展参数 */
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;
    outputs[1].want_float = 1;
    ret = rknn_outputs_get(g_rknn_ctx, 2, outputs, NULL);
    if (ret < 0) {
        printf("[ocr_engine] rknn_outputs_get 失败: %d\n", ret);
        free(resized);
        return -1;
    }

    /* ---- 6. 后处理：解码输出为文字 ----
     * CRNN 输出形状通常为 [time_steps, char_classes]，
     * 需要 CTC 解码：取每个时间步 argmax，去除 blank 与重复。
     * 此处仅做占位演示，真实项目应实现 CTC 解码或调用模型配套后处理。 */
    snprintf(output, ENGINE_OUTPUT_MAX,
             "[OCR 识别结果占位] 模型输出已取回，"
             "请根据具体模型实现 CTC 解码逻辑。");

    /* ---- 7. 释放输出张量占用的内存 ----
     * 每次 rknn_outputs_get 后必须调用 rknn_outputs_release 释放
     * 否则会内存泄漏，影响长时间稳定性 */
    rknn_outputs_release(g_rknn_ctx, 2, outputs);

    free(resized);
    printf("[ocr_engine] 识别完成\n");
    return 0;
}

/*
 * 接口 3：ocr_engine_destroy —— 销毁引擎
 */
void ocr_engine_destroy(void)
{
    if (!g_initialized) {
        return;
    }

    /* rknn_destroy 销毁上下文，释放 NPU 内部资源
     * 注意：输入/输出缓冲区由调用方管理，需自行 free */
    rknn_destroy(g_rknn_ctx);
    g_rknn_ctx = 0;

    g_initialized = 0;
    printf("[ocr_engine] 资源已释放\n");
}


/* =========================================================================
 * 条件编译：Mock 桩实现（无 RKNN Lite 时编译，PC 环境使用）
 * ========================================================================= */
#elif defined(HAS_ONNXRUNTIME)

/*
 * =========================================================================
 * 条件编译：ONNX Runtime 完整 PP-OCR 推理实现（Ubuntu x86 虚拟机 CPU 推理）
 * =========================================================================
 * 【适用场景】
 *   PC / Ubuntu 虚拟机环境，无 NPU，通过 ONNX Runtime CPU 后端运行 PP-OCR。
 *
 * 【模型文件】（固定路径，init 时统一加载）
 *   ./models/ocr/det.onnx           —— 文本检测模型（DBNet，输出概率图）
 *   ./models/ocr/cls.onnx           —— 方向分类模型（0° / 180° 二分类）
 *   ./models/ocr/rec.onnx           —— 文字识别模型（CRNN + CTC 解码）
 *   ./models/ocr/ppocr_keys_v1.txt  —— 识别字典（可选，缺失时用内置最小字典）
 *
 * 【推理流水线（run 函数内部完整执行）】
 *   ① det 检测：整图 → det.onnx → 概率图 → 阈值化 + 连通域 → 文本框列表
 *   ② cls 分类：逐框裁剪 → cls.onnx → 0°/180° 判定 → 必要时旋转 180° 矫正
 *   ③ rec 识别：矫正后裁剪 → rec.onnx → CTC 解码 → 拼接文字
 *   ④ 汇总所有文本框识别结果到 output 缓冲区（每框一行）
 *
 * 【内存管理】
 *   全局静态变量存储 ORT 环境 + 三个 Session，init 时创建，destroy 时完整释放。
 *   每次 run 内部的临时缓冲区（input_data 等）用 malloc/free 即时回收，无泄漏。
 * =========================================================================
 */

/* ---- ORT API 错误检查宏 ----
 * ORT C API 每个调用返回 OrtStatus*，NULL=成功，非 NULL=出错。
 * 本宏检查返回值，出错时打印错误信息并释放 status 对象。 */
#define ORT_CHECK(status_call) do { \
    OrtStatus* _ort_status = (status_call); \
    if (_ort_status) { \
        printf("[ocr_engine ORT] 错误: %s\n", g_ort->GetErrorMessage(_ort_status)); \
        g_ort->ReleaseStatus(_ort_status); \
    } \
} while(0)

/* ---- 全局 ORT 状态（静态，仅本文件可见）---- */
static const OrtApi* g_ort       = NULL;  /* ORT API 函数表入口（所有调用的根） */
static OrtEnv*       g_ort_env   = NULL;  /* ORT 运行环境（全局唯一，进程级） */
static OrtSession*   g_det_sess  = NULL;  /* 检测模型会话 */
static OrtSession*   g_cls_sess  = NULL;  /* 分类模型会话 */
static OrtSession*   g_rec_sess  = NULL;  /* 识别模型会话 */
static int           g_ort_inited = 0;    /* 初始化完成标志 */

/* ---- PP-OCR 模型参数常量 ---- */
#define DET_TARGET_SIZE   960    /* 检测模型输入长边上限（过大耗内存，过小漏检） */
#define CLS_INPUT_H       48     /* 分类模型固定输入高度 */
#define CLS_INPUT_W       192    /* 分类模型固定输入宽度 */
#define REC_INPUT_H       48     /* 识别模型固定输入高度（PP-OCRv3=48） */
#define REC_MAX_W         1024   /* 识别模型输入最大宽度（超过则截断） */
#define DET_THRESH        0.3f   /* 检测概率图二值化阈值（>0.3 视为文本区域） */
#define DET_MIN_BOX_AREA  100    /* 最小文本框面积（过滤噪点，太小框无有效文字） */
#define MAX_BOXES         256    /* 单张图片最多检测的文本框数量 */

/* ---- 识别字典（CTC 解码用）----
 * PP-OCR 字典每行一个字符，索引 0 对应 CTC blank（空白标签）。
 * 中文模型约 6624 个字符，这里预分配 7000 个槽位，每个字符最多 15 字节。 */
static char g_rec_dict[7000][16];  /* 字典字符数组（UTF-8 编码，支持中文） */
static int  g_rec_dict_size = 0;   /* 实际加载的字符数量 */

/*
 * ort_load_dict —— 加载识别字典文件
 * 【参数】path：字典文件路径（每行一个字符，UTF-8 编码）
 * 【返回值】0=成功（文件不存在时自动用内置最小字典，也返回 0）
 * 【原理】逐行读取文件，去除换行符后存入全局字典数组。
 *        文件不存在时不报错，改用内置最小字典（数字+字母+标点），
 *        保证无字典文件也能基本运行。
 */
static int ort_load_dict(const char* path)
{
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        /* 字典文件不存在，使用内置最小字典 */
        printf("[ocr_engine ORT] 字典文件未找到: %s，使用内置最小字典\n", path);
        const char* default_chars[] = {
            "blank",
            "0","1","2","3","4","5","6","7","8","9",
            "a","b","c","d","e","f","g","h","i","j","k","l","m",
            "n","o","p","q","r","s","t","u","v","w","x","y","z",
            "A","B","C","D","E","F","G","H","I","J","K","L","M",
            "N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
            " ",".",",","!","?",":",";","(",")","-","/","'","\""
        };
        int n = (int)(sizeof(default_chars) / sizeof(default_chars[0]));
        int i;
        for (i = 0; i < n && i < 7000; i++) {
            strncpy(g_rec_dict[i], default_chars[i], 15);
            g_rec_dict[i][15] = '\0';
        }
        g_rec_dict_size = n;
        return 0;
    }

    /* 逐行读取字典文件
     * PP-OCR rec 模型 CTC 解码规则：
     *   索引 0 = blank（CTC 空白标签，不对应字典文件任何行）
     *   索引 1 = 字典文件第 1 行
     *   索引 2 = 字典文件第 2 行
     *   ...
     * 因此 g_rec_dict[0] 手动设为 "blank"，字典文件从 g_rec_dict[1] 开始存 */
    char line[32];
    strcpy(g_rec_dict[0], "blank");
    g_rec_dict_size = 1;
    while (fgets(line, sizeof(line), fp) != NULL && g_rec_dict_size < 7000) {
        /* 去除行尾换行符（\n 和 \r） */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        /* PaddleOCR 字典第一行通常是空行（代表空格），不应跳过 */
        if (len == 0) {
            strncpy(g_rec_dict[g_rec_dict_size], " ", 15);
        } else {
            strncpy(g_rec_dict[g_rec_dict_size], line, 15);
        }
        g_rec_dict[g_rec_dict_size][15] = '\0';
        g_rec_dict_size++;
    }
    fclose(fp);
    printf("[ocr_engine ORT] 字典已加载: %d 个字符（含 1 个 blank）\n", g_rec_dict_size);
    return 0;
}

/*
 * ort_create_session —— 创建 ORT 推理会话（内部辅助函数）
 * 【参数】model_path：ONNX 模型文件路径
 * 【返回值】OrtSession* 指针（成功），NULL（失败）
 * 【原理】创建 SessionOptions → 设置线程数与图优化 → 从文件加载模型 → 返回会话
 *        线程数设为 1，避免虚拟机 CPU 过载；图优化设为最高级别 ORT_ENABLE_ALL。
 */
static OrtSession* ort_create_session(const char* model_path)
{
    OrtSession* session = NULL;
    OrtSessionOptions* options = NULL;

    /* 创建会话选项对象 */
    ORT_CHECK(g_ort->CreateSessionOptions(&options));
    if (options == NULL) {
        return NULL;
    }

    /* 设置 CPU 推理线程数为 1（虚拟机环境稳妥配置） */
    ORT_CHECK(g_ort->SetIntraOpNumThreads(options, 1));
    /* 启用全部图优化（常量折叠、算子融合等，提升推理速度） */
    ORT_CHECK(g_ort->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));

    /* 从文件创建推理会话 */
    OrtStatus* status = g_ort->CreateSession(g_ort_env, model_path, options, &session);
    if (status) {
        printf("[ocr_engine ORT] 加载模型失败: %s -> %s\n",
               model_path, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        g_ort->ReleaseSessionOptions(options);
        return NULL;
    }

    g_ort->ReleaseSessionOptions(options);
    printf("[ocr_engine ORT] 模型已加载: %s\n", model_path);
    return session;
}

/*
 * ort_normalize_hwc —— 对 cv::Mat 图片做标准化（原地操作）
 * 【参数】img：CV_32FC3 格式图片（已除以 255 归一化到 [0,1]）
 * 【参数】mean：三通道均值数组（如 {0.485, 0.456, 0.406}）
 * 【参数】std：三通道标准差数组（如 {0.229, 0.224, 0.225}）
 * 【原理】对每个像素：(pixel - mean) / std，使数据分布接近标准正态。
 *        这是 PP-OCR 三个模型统一的预处理步骤。
 */
static void ort_normalize_hwc(cv::Mat& img, const float mean[3], const float std_v[3])
{
    int i, j, c;
    for (i = 0; i < img.rows; i++) {
        for (j = 0; j < img.cols; j++) {
            float* pixel = img.ptr<float>(i, j);
            for (c = 0; c < 3; c++) {
                pixel[c] = (pixel[c] - mean[c]) / std_v[c];
            }
        }
    }
}

/*
 * ort_hwc_to_chw —— 将 HWC 格式数据转换为 CHW 格式
 * 【参数】src：HWC 格式源数据（height × width × channels）
 * 【参数】dst：CHW 格式目标缓冲区（channels × height × width，需预分配）
 * 【参数】h, w, c：图片的高、宽、通道数
 * 【原理】OpenCV 默认 HWC（高×宽×通道），ONNX 模型通常要求 CHW（通道×高×宽）。
 *        本函数做内存布局转置：dst[c*h*w + i*w + j] = src[i*w*c + j*c + c_idx]
 */
static void ort_hwc_to_chw(const float* src, float* dst, int h, int w, int c)
{
    int i, j, ch;
    for (ch = 0; ch < c; ch++) {
        for (i = 0; i < h; i++) {
            for (j = 0; j < w; j++) {
                dst[ch * h * w + i * w + j] = src[(i * w + j) * c + ch];
            }
        }
    }
}

/*
 * ort_run_det —— 运行文本检测模型（det.onnx）
 * 【参数】img：输入图片（BGR 格式，cv::Mat）
 * 【参数】boxes：输出文本框数组（调用方预分配，容量 max_boxes）
 * 【参数】max_boxes：boxes 数组最大容量
 * 【返回值】检测到的文本框数量（≥0），-1=失败
 * 【流程】
 *   1. 等比缩放图片到长边≤960，对齐到 32 的倍数
 *   2. BGR→RGB，归一化到 [0,1]，标准化（mean/std）
 *   3. HWC→CHW，创建 ORT 输入张量
 *   4. 调用 det.onnx 推理，获取概率图输出
 *   5. 阈值化（>0.3 → 255），放大到原图尺寸
 *   6. cv::findContours 找连通域，cv::boundingRect 取外接矩形
 *   7. 过滤小框，按从上到下、从左到右排序
 */
static int ort_run_det(cv::Mat img, cv::Rect* boxes, int max_boxes)
{
    int orig_h = img.rows;
    int orig_w = img.cols;

    /* ---- 1. 等比缩放 + 对齐到 32 的倍数 ---- */
    float ratio = (float)DET_TARGET_SIZE / (orig_h > orig_w ? orig_h : orig_w);
    if (ratio > 1.0f) ratio = 1.0f;
    int new_h = (int)(orig_h * ratio);
    int new_w = (int)(orig_w * ratio);
    new_h = (new_h + 31) / 32 * 32;
    new_w = (new_w + 31) / 32 * 32;

    cv::Mat resized;
    cv::resize(img, resized, cv::Size(new_w, new_h));

    /* ---- 2. BGR→RGB + 归一化 + 标准化 ---- */
    /* PaddleOCR 官方预处理用 BGR（OpenCV 默认），不做 BGR→RGB 转换 */
    resized.convertTo(resized, CV_32FC3, 1.0 / 255.0);
    float det_mean[] = {0.485f, 0.456f, 0.406f};
    float det_std[]  = {0.229f, 0.224f, 0.225f};
    ort_normalize_hwc(resized, det_mean, det_std);

    /* ---- 3. HWC → CHW 转换 ---- */
    int input_count = 3 * new_h * new_w;
    float* input_data = (float*)malloc(input_count * sizeof(float));
    if (input_data == NULL) {
        printf("[ocr_engine ORT] det 输入缓冲区分配失败\n");
        return -1;
    }
    ort_hwc_to_chw((const float*)resized.data, input_data, new_h, new_w, 3);

    int64_t input_shape[] = {1, 3, new_h, new_w};

    /* ---- 4. 创建 ORT 输入张量 ---- */
    OrtMemoryInfo* mem_info = NULL;
    ORT_CHECK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info));
    OrtValue* input_tensor = NULL;
    ORT_CHECK(g_ort->CreateTensorWithDataAsOrtValue(
        mem_info, input_data, (size_t)input_count * sizeof(float),
        input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor));

    /* 获取模型输入/输出名称 */
    OrtAllocator* allocator = NULL;
    g_ort->GetAllocatorWithDefaultOptions(&allocator);
    char* input_name = NULL;
    g_ort->SessionGetInputName(g_det_sess, 0, allocator, &input_name);
    char* output_name = NULL;
    g_ort->SessionGetOutputName(g_det_sess, 0, allocator, &output_name);

    /* ---- 5. 运行推理 ---- */
    const char* input_names[]  = { input_name };
    const char* output_names[] = { output_name };
    OrtValue* output_tensor = NULL;

    OrtStatus* status = g_ort->Run(g_det_sess, NULL,
                                    input_names, (const OrtValue* const*)&input_tensor, 1,
                                    output_names, 1, &output_tensor);
    if (status) {
        printf("[ocr_engine ORT] det Run 失败: %s\n", g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        g_ort->AllocatorFree(allocator, input_name);
        g_ort->AllocatorFree(allocator, output_name);
        g_ort->ReleaseValue(input_tensor);
        g_ort->ReleaseMemoryInfo(mem_info);
        free(input_data);
        return -1;
    }

    /* ---- 6. 获取输出概率图 ---- */
    float* prob_map = NULL;
    g_ort->GetTensorMutableData(output_tensor, (void**)&prob_map);

    /* 查询输出形状以确定概率图尺寸 */
    OrtTensorTypeAndShapeInfo* shape_info = NULL;
    g_ort->GetTensorTypeAndShape(output_tensor, &shape_info);
    size_t dim_count = 0;
    g_ort->GetDimensionsCount(shape_info, &dim_count);
    int64_t out_shape[4] = {0};
    if (dim_count <= 4) {
        g_ort->GetDimensions(shape_info, out_shape, dim_count);
    }
    g_ort->ReleaseTensorTypeAndShapeInfo(shape_info);

    /* 输出形状通常为 [1, 1, H, W]，取最后两维作为概率图高宽 */
    int out_h = (dim_count >= 2) ? (int)out_shape[dim_count - 2] : new_h;
    int out_w = (dim_count >= 2) ? (int)out_shape[dim_count - 1] : new_w;

    /* ---- 7. 后处理：sigmoid → 灰度图 → Otsu 自适应阈值 → 连通域 → 外接矩形 ---- */
    /* PaddleOCR det 模型输出的是 logits（原始分数），不是概率值
     * 需要先做 sigmoid 变换：prob = 1 / (1 + exp(-logit))
     * 然后用 Otsu 大津法自动计算最佳阈值，避免固定阈值对不同图片效果差异大 */
    cv::Mat prob_gray(out_h, out_w, CV_8UC1);   /* 概率灰度图（0~255） */
    int i, j;
    for (i = 0; i < out_h; i++) {
        for (j = 0; j < out_w; j++) {
            float logit = prob_map[i * out_w + j];
            float prob = 1.0f / (1.0f + expf(-logit));
            prob_gray.at<unsigned char>(i, j) = (unsigned char)(prob * 255);
        }
    }

    /* Otsu 自动阈值：cv::threshold 返回计算出的阈值（0~255）
     * THRESH_OTSU 表示用大津法自动计算阈值，THRESH_BINARY 表示二值化
     * 相比固定阈值，Otsu 能根据概率图直方图自动找到类间方差最大的分界点 */
    cv::Mat binary;
    cv::threshold(prob_gray, binary, 0, 255,
                  cv::THRESH_BINARY | cv::THRESH_OTSU);

    /* 放大二值图到原始图片尺寸（便于获取原图坐标的文本框） */
    cv::Mat binary_full;
    cv::resize(binary, binary_full, cv::Size(orig_w, orig_h), 0, 0, cv::INTER_NEAREST);

    /* 查找连通域轮廓 */
    std::vector<std::vector<cv::Point> > contours;
    cv::findContours(binary_full, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

    /* 转为外接矩形并过滤小框 + 过滤异常大框
     * 异常大框（> 图片面积 50%）通常是二值化全白导致的误检，
     * 不是真正的文本行——真正文本行面积一般 < 图片面积 20% */
    int img_area = orig_w * orig_h;
    int box_count = 0;
    for (i = 0; i < (int)contours.size() && box_count < max_boxes; i++) {
        cv::Rect rect = cv::boundingRect(contours[i]);
        int rect_area = rect.width * rect.height;
        if (rect_area < DET_MIN_BOX_AREA) {
            continue;   /* 过滤太小的框（噪点） */
        }
        if (rect_area > img_area / 2) {
            printf("[ocr_engine ORT] 跳过异常大框: w=%d h=%d (面积占比=%.0f%%)\n",
                   rect.width, rect.height, 100.0f * rect_area / img_area);
            continue;   /* 过滤异常大框（> 图片面积 50%） */
        }
        /* 限制在图片范围内 */
        rect.x = (rect.x < 0) ? 0 : rect.x;
        rect.y = (rect.y < 0) ? 0 : rect.y;
        rect.width  = (rect.x + rect.width  > orig_w) ? orig_w - rect.x : rect.width;
        rect.height = (rect.y + rect.height > orig_h) ? orig_h - rect.y : rect.height;
        if (rect.width > 0 && rect.height > 0) {
            boxes[box_count++] = rect;
        }
    }

    /* 按从上到下、从左到右排序（简单冒泡） */
    for (i = 0; i < box_count - 1; i++) {
        int j2;
        for (j2 = i + 1; j2 < box_count; j2++) {
            if (boxes[j2].y < boxes[i].y ||
                (boxes[j2].y == boxes[i].y && boxes[j2].x < boxes[i].x)) {
                cv::Rect tmp = boxes[i];
                boxes[i] = boxes[j2];
                boxes[j2] = tmp;
            }
        }
    }

    printf("[ocr_engine ORT] 检测到 %d 个文本框\n", box_count);

    /* ---- 8. 释放资源 ---- */
    g_ort->AllocatorFree(allocator, input_name);
    g_ort->AllocatorFree(allocator, output_name);
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseValue(output_tensor);
    g_ort->ReleaseMemoryInfo(mem_info);
    free(input_data);

    return box_count;
}

/*
 * ort_run_cls —— 运行方向分类模型（cls.onnx）
 * 【参数】crop：裁剪的文本区域图片（BGR 格式）
 * 【返回值】0=正常（0°，无需旋转），1=需旋转 180°，-1=失败
 * 【原理】cls 模型输出 2 个概率值：out[0]=0° 概率，out[1]=180° 概率。
 *        若 180° 概率更大，则文本倒置，需要旋转矫正。
 */
static int ort_run_cls(cv::Mat crop)
{
    /* ---- 1. 缩放到固定尺寸 + 归一化 ---- */
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(CLS_INPUT_W, CLS_INPUT_H));
    /* PaddleOCR 官方预处理用 BGR（OpenCV 默认），不做 BGR→RGB 转换 */
    resized.convertTo(resized, CV_32FC3, 1.0 / 255.0);
    float cls_mean[] = {0.485f, 0.456f, 0.406f};
    float cls_std[]  = {0.229f, 0.224f, 0.225f};
    ort_normalize_hwc(resized, cls_mean, cls_std);

    /* HWC → CHW */
    int input_count = 3 * CLS_INPUT_H * CLS_INPUT_W;
    float* input_data = (float*)malloc(input_count * sizeof(float));
    if (input_data == NULL) return -1;
    ort_hwc_to_chw((const float*)resized.data, input_data, CLS_INPUT_H, CLS_INPUT_W, 3);

    int64_t input_shape[] = {1, 3, CLS_INPUT_H, CLS_INPUT_W};

    /* ---- 2. 创建张量并运行 ---- */
    OrtMemoryInfo* mem_info = NULL;
    ORT_CHECK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info));
    OrtValue* input_tensor = NULL;
    ORT_CHECK(g_ort->CreateTensorWithDataAsOrtValue(
        mem_info, input_data, (size_t)input_count * sizeof(float),
        input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor));

    OrtAllocator* allocator = NULL;
    g_ort->GetAllocatorWithDefaultOptions(&allocator);
    char* input_name = NULL;
    g_ort->SessionGetInputName(g_cls_sess, 0, allocator, &input_name);
    char* output_name = NULL;
    g_ort->SessionGetOutputName(g_cls_sess, 0, allocator, &output_name);

    const char* input_names[]  = { input_name };
    const char* output_names[] = { output_name };
    OrtValue* output_tensor = NULL;

    OrtStatus* status = g_ort->Run(g_cls_sess, NULL,
                                    input_names, (const OrtValue* const*)&input_tensor, 1,
                                    output_names, 1, &output_tensor);

    int need_rotate = -1;
    if (status == NULL && output_tensor) {
        /* 获取输出（2 个浮点数：0° 概率 和 180° 概率） */
        float* out = NULL;
        g_ort->GetTensorMutableData(output_tensor, (void**)&out);
        if (out) {
            need_rotate = (out[1] > out[0]) ? 1 : 0;
            printf("[ocr_engine ORT] 方向分类: %s\n",
                   need_rotate ? "180°(需旋转)" : "0°(正常)");
        }
    } else if (status) {
        printf("[ocr_engine ORT] cls Run 失败: %s\n", g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
    }

    /* 释放资源 */
    g_ort->AllocatorFree(allocator, input_name);
    g_ort->AllocatorFree(allocator, output_name);
    if (output_tensor) g_ort->ReleaseValue(output_tensor);
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseMemoryInfo(mem_info);
    free(input_data);

    return need_rotate;
}

/*
 * ort_run_rec —— 运行文字识别模型（rec.onnx）
 * 【参数】crop：裁剪的文本区域图片（BGR 格式，已矫正方向）
 * 【参数】out_text：输出识别文字字符串
 * 【参数】max_len：out_text 缓冲区最大长度
 * 【返回值】0=成功，-1=失败
 * 【原理】
 *   1. 等比缩放到高度=32，宽度按比例计算（上限 320，对齐到 4 的倍数）
 *   2. BGR→RGB，归一化，标准化（rec 模型用 mean=0.5, std=0.5）
 *   3. HWC→CHW，创建张量，运行推理
 *   4. CTC 解码：取每个时间步 argmax，跳过 blank(索引0) 和重复字符
 */
static int ort_run_rec(cv::Mat crop, char* out_text, int max_len)
{
    int orig_w = crop.cols;
    int orig_h = crop.rows;

    /* ---- 1. 等比缩放到高度=32 ---- */
    float ratio = (float)REC_INPUT_H / orig_h;
    int new_w = (int)(orig_w * ratio);
    if (new_w > REC_MAX_W) new_w = REC_MAX_W;
    new_w = (new_w + 3) / 4 * 4;  /* 对齐到 4 的倍数 */
    if (new_w < 4) new_w = 4;

    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(new_w, REC_INPUT_H));
    /* PaddleOCR 官方预处理用 BGR（OpenCV 默认），不做 BGR→RGB 转换 */
    resized.convertTo(resized, CV_32FC3, 1.0 / 255.0);
    float rec_mean[] = {0.5f, 0.5f, 0.5f};
    float rec_std[]  = {0.5f, 0.5f, 0.5f};
    ort_normalize_hwc(resized, rec_mean, rec_std);

    /* HWC → CHW */
    int input_count = 3 * REC_INPUT_H * new_w;
    float* input_data = (float*)malloc(input_count * sizeof(float));
    if (input_data == NULL) return -1;
    ort_hwc_to_chw((const float*)resized.data, input_data, REC_INPUT_H, new_w, 3);

    int64_t input_shape[] = {1, 3, REC_INPUT_H, new_w};

    /* ---- 2. 创建张量并运行 ---- */
    OrtMemoryInfo* mem_info = NULL;
    ORT_CHECK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info));
    OrtValue* input_tensor = NULL;
    ORT_CHECK(g_ort->CreateTensorWithDataAsOrtValue(
        mem_info, input_data, (size_t)input_count * sizeof(float),
        input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor));

    OrtAllocator* allocator = NULL;
    g_ort->GetAllocatorWithDefaultOptions(&allocator);
    char* input_name = NULL;
    g_ort->SessionGetInputName(g_rec_sess, 0, allocator, &input_name);
    char* output_name = NULL;
    g_ort->SessionGetOutputName(g_rec_sess, 0, allocator, &output_name);

    const char* input_names[]  = { input_name };
    const char* output_names[] = { output_name };
    OrtValue* output_tensor = NULL;

    OrtStatus* status = g_ort->Run(g_rec_sess, NULL,
                                    input_names, (const OrtValue* const*)&input_tensor, 1,
                                    output_names, 1, &output_tensor);

    out_text[0] = '\0';
    if (status == NULL && output_tensor) {
        /* ---- 3. 获取输出形状 ---- */
        OrtTensorTypeAndShapeInfo* shape_info = NULL;
        g_ort->GetTensorTypeAndShape(output_tensor, &shape_info);
        size_t dim_count = 0;
        g_ort->GetDimensionsCount(shape_info, &dim_count);
        int64_t out_shape[4] = {0};
        if (dim_count <= 4) {
            g_ort->GetDimensions(shape_info, out_shape, dim_count);
        }
        g_ort->ReleaseTensorTypeAndShapeInfo(shape_info);

        /* 输出形状可能为以下几种（取决于模型导出方式）：
         *   [batch, time_steps, num_chars]  —— 3 维，最常见（PP-OCR 默认）
         *   [time_steps, num_chars]         —— 2 维
         *   [batch, time_steps, 1, num_chars] —— 4 维
         * 统一取倒数第二维为 time_steps，最后一维为 num_chars */
        int time_steps = 0;
        int num_chars  = 0;
        if (dim_count >= 2) {
            time_steps = (int)out_shape[dim_count - 2];
            num_chars  = (int)out_shape[dim_count - 1];
        }
        /* 获取输出数据 */
        float* out = NULL;
        g_ort->GetTensorMutableData(output_tensor, (void**)&out);

        if (out && time_steps > 0 && num_chars > 0) {
            /* ---- 4. CTC 解码 ----
             * 规则：取每个时间步 argmax 索引，
             *       跳过 blank（索引 0）和与前一个相同的重复索引 */
            int text_pos = 0;
            int last_idx = 0;  /* 上一个时间步的索引，初始化为 blank */
            int t;
            for (t = 0; t < time_steps; t++) {
                /* 找当前时间步概率最大的字符索引 */
                float max_val = -1e9f;
                int max_idx = 0;
                int k;
                for (k = 0; k < num_chars; k++) {
                    float val = out[t * num_chars + k];
                    if (val > max_val) {
                        max_val = val;
                        max_idx = k;
                    }
                }
                /* CTC 解码：跳过 blank 和重复 */
                if (max_idx != 0 && max_idx != last_idx) {
                    if (max_idx < g_rec_dict_size) {
                        int char_len = (int)strlen(g_rec_dict[max_idx]);
                        if (text_pos + char_len < max_len - 1) {
                            memcpy(out_text + text_pos, g_rec_dict[max_idx], char_len);
                            text_pos += char_len;
                        }
                    }
                }
                last_idx = max_idx;
            }
            out_text[text_pos] = '\0';
        }
    } else if (status) {
        printf("[ocr_engine ORT] rec Run 失败: %s\n", g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
    }

    /* 释放资源 */
    g_ort->AllocatorFree(allocator, input_name);
    g_ort->AllocatorFree(allocator, output_name);
    if (output_tensor) g_ort->ReleaseValue(output_tensor);
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseMemoryInfo(mem_info);
    free(input_data);

    return 0;
}

/*
 * =============================================================================
 * 接口实现：init / run / destroy
 * =============================================================================
 */

/*
 * ocr_engine_init —— 加载三个 ONNX 模型并初始化 ORT 环境
 * 【参数】model_path：模型目录路径（NULL 则使用默认 ./models/ocr/）
 * 【返回值】0=成功，-1=失败
 * 【流程】
 *   1. 获取 ORT API 函数表
 *   2. 创建 ORT 运行环境
 *   3. 依次加载 det.onnx / cls.onnx / rec.onnx 三个模型
 *   4. 加载识别字典 ppocr_keys_v1.txt
 *   任一步骤失败则回滚已分配资源并返回 -1。
 */
int ocr_engine_init(const char *model_path)
{
    if (g_ort_inited) {
        printf("[ocr_engine ORT] 已初始化，请先 destroy\n");
        return -1;
    }

    /* 确定模型目录 */
    char base_dir[512];
    if (model_path != NULL) {
        strncpy(base_dir, model_path, sizeof(base_dir) - 1);
        base_dir[sizeof(base_dir) - 1] = '\0';
        /* 去除末尾斜杠 */
        int len = (int)strlen(base_dir);
        if (len > 0 && base_dir[len - 1] == '/') {
            base_dir[len - 1] = '\0';
        }
    } else {
        strncpy(base_dir, "./models/ocr", sizeof(base_dir) - 1);
        base_dir[sizeof(base_dir) - 1] = '\0';
    }

    /* 拼接三个模型文件路径 */
    char det_path[640], cls_path[640], rec_path[640];
    snprintf(det_path, sizeof(det_path), "%s/det.onnx", base_dir);
    snprintf(cls_path, sizeof(cls_path), "%s/cls.onnx", base_dir);
    snprintf(rec_path, sizeof(rec_path), "%s/rec.onnx", base_dir);

    printf("[ocr_engine ORT] 初始化 ONNX Runtime PP-OCR\n");
    printf("[ocr_engine ORT] 模型目录: %s\n", base_dir);

    /* ---- 1. 获取 ORT API 函数表 ---- */
    const OrtApiBase* api_base = OrtGetApiBase();
    if (api_base == NULL) {
        printf("[ocr_engine ORT] 获取 API Base 失败\n");
        return -1;
    }
    g_ort = api_base->GetApi(ORT_API_VERSION);
    if (g_ort == NULL) {
        printf("[ocr_engine ORT] 获取 API 失败（ORT_API_VERSION=%d）\n", ORT_API_VERSION);
        return -1;
    }
    printf("[ocr_engine ORT] ORT 版本: %s\n", api_base->GetVersionString());

    /* ---- 2. 创建 ORT 运行环境 ---- */
    ORT_CHECK(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "EdgeSim", &g_ort_env));
    if (g_ort_env == NULL) {
        printf("[ocr_engine ORT] 创建运行环境失败\n");
        return -1;
    }

    /* ---- 3. 加载三个模型（任一失败则回滚）---- */
    g_det_sess = ort_create_session(det_path);
    if (g_det_sess == NULL) {
        printf("[ocr_engine ORT] 检测模型加载失败，初始化中止\n");
        g_ort->ReleaseEnv(g_ort_env);
        g_ort_env = NULL;
        g_ort = NULL;
        return -1;
    }

    g_cls_sess = ort_create_session(cls_path);
    if (g_cls_sess == NULL) {
        printf("[ocr_engine ORT] 分类模型加载失败，初始化中止\n");
        g_ort->ReleaseSession(g_det_sess);
        g_det_sess = NULL;
        g_ort->ReleaseEnv(g_ort_env);
        g_ort_env = NULL;
        g_ort = NULL;
        return -1;
    }

    g_rec_sess = ort_create_session(rec_path);
    if (g_rec_sess == NULL) {
        printf("[ocr_engine ORT] 识别模型加载失败，初始化中止\n");
        g_ort->ReleaseSession(g_cls_sess);
        g_ort->ReleaseSession(g_det_sess);
        g_cls_sess = NULL;
        g_det_sess = NULL;
        g_ort->ReleaseEnv(g_ort_env);
        g_ort_env = NULL;
        g_ort = NULL;
        return -1;
    }

    /* ---- 4. 加载识别字典 ---- */
    char dict_path[640];
    snprintf(dict_path, sizeof(dict_path), "%s/ppocr_keys_v1.txt", base_dir);
    ort_load_dict(dict_path);

    g_ort_inited = 1;
    printf("[ocr_engine ORT] 初始化完成（det + cls + rec 三模型就绪）\n");
    return 0;
}

/*
 * ocr_engine_run —— 执行一次完整 OCR 识别（检测→分类→识别）
 * 【参数】input：图片文件路径（jpg/png 等 OpenCV 支持的格式）
 * 【参数】output：输出缓冲区，存放识别到的文字（每个文本框一行）
 * 【返回值】0=成功，-1=失败
 */
int ocr_engine_run(const char *input, char *output)
{
    if (!g_ort_inited || input == NULL || output == NULL) {
        return -1;
    }

    printf("[ocr_engine ORT] 开始识别: %s\n", input);

    /* ---- 1. 使用 OpenCV 读取图片 ---- */
    cv::Mat img = cv::imread(input, cv::IMREAD_COLOR);
    if (img.empty()) {
        printf("[ocr_engine ORT] 图片读取失败: %s\n", input);
        return -1;
    }
    printf("[ocr_engine ORT] 图片尺寸: %dx%d\n", img.cols, img.rows);

    /* ---- 2. 文本检测 ---- */
    cv::Rect boxes[MAX_BOXES];
    int box_count = ort_run_det(img, boxes, MAX_BOXES);
    if (box_count <= 0) {
        snprintf(output, ENGINE_OUTPUT_MAX, "[未检测到文本]");
        printf("[ocr_engine ORT] 未检测到文本\n");
        return 0;
    }

    /* ---- 3. 逐框：分类矫正 + 文字识别 ---- */
    int output_pos = 0;
    int i;
    for (i = 0; i < box_count; i++) {
        /* 膨胀检测框：按高度 20% 自适应膨胀，确保完整文字被包含
         * det 的 Otsu 阈值可能偏高，文字边缘被截断，膨胀后能包含完整偏旁部首
         * 用相对比例而非固定像素，适配不同大小的文字 */
        int pad = boxes[i].height / 5;   /* 高度的 20% */
        if (pad < 10) pad = 10;           /* 最小 10 像素 */
        boxes[i].x = (boxes[i].x > pad) ? boxes[i].x - pad : 0;
        boxes[i].y = (boxes[i].y > pad) ? boxes[i].y - pad : 0;
        boxes[i].width  += 2 * pad;
        boxes[i].height += 2 * pad;
        /* 限制在图片范围内 */
        if (boxes[i].x + boxes[i].width > img.cols) boxes[i].width = img.cols - boxes[i].x;
        if (boxes[i].y + boxes[i].height > img.rows) boxes[i].height = img.rows - boxes[i].y;

        /* 裁剪文本区域 */
        cv::Mat crop = img(boxes[i]).clone();

        /* 方向分类（判断是否需要旋转 180°） */
        int need_rotate = ort_run_cls(crop);
        if (need_rotate == 1) {
            cv::rotate(crop, crop, cv::ROTATE_180);
            printf("[ocr_engine ORT] 框 %d: 已旋转 180°\n", i);
        }

        /* 文字识别 */
        char rec_text[1024];
        rec_text[0] = '\0';
        ort_run_rec(crop, rec_text, sizeof(rec_text));

        printf("[ocr_engine ORT] 框 %d 识别结果: \"%s\"\n", i, rec_text);

        /* 拼接到输出（每框一行，用换行符分隔） */
        int text_len = (int)strlen(rec_text);
        if (output_pos + text_len + 2 < ENGINE_OUTPUT_MAX) {
            memcpy(output + output_pos, rec_text, text_len);
            output_pos += text_len;
            output[output_pos++] = '\n';
        }
    }

    /* 去除末尾多余的换行符 */
    if (output_pos > 0 && output[output_pos - 1] == '\n') {
        output_pos--;
    }
    output[output_pos] = '\0';

    printf("[ocr_engine ORT] 识别完成，共 %d 个文本框\n", box_count);
    return 0;
}

/*
 * ocr_engine_destroy —— 销毁引擎，释放所有 ORT 资源
 * 【原理】按加载的逆序释放：rec → cls → det → env。
 *        每个 ReleaseSession 释放模型内存与推理上下文，
 *        ReleaseEnv 释放全局运行环境。
 */
void ocr_engine_destroy(void)
{
    if (!g_ort_inited) {
        return;
    }

    /* 按加载逆序释放 */
    if (g_rec_sess) {
        g_ort->ReleaseSession(g_rec_sess);
        g_rec_sess = NULL;
    }
    if (g_cls_sess) {
        g_ort->ReleaseSession(g_cls_sess);
        g_cls_sess = NULL;
    }
    if (g_det_sess) {
        g_ort->ReleaseSession(g_det_sess);
        g_det_sess = NULL;
    }
    if (g_ort_env) {
        g_ort->ReleaseEnv(g_ort_env);
        g_ort_env = NULL;
    }

    g_ort = NULL;
    g_ort_inited = 0;
    printf("[ocr_engine ORT] 所有 ORT 资源已释放\n");
}


#else  /* 未定义RKNN、未定义ONNX，进入Mock模拟桩 */

static int g_mock_initialized = 0;

int ocr_engine_init(const char *model_path)
{
    if (model_path == NULL) {
        return -1;
    }
    if (g_mock_initialized) {
        return -1;
    }
    /* Mock 模式：不真正加载模型，仅打印提示 */
    printf("[ocr_engine MOCK] 模拟加载 RKNN 模型: %s\n", model_path);
    printf("[ocr_engine MOCK] (未定义 HAS_RKNN_LITE，使用 mock 桩)\n");
    g_mock_initialized = 1;
    return 0;
}

int ocr_engine_run(const char *input, char *output)
{
    if (!g_mock_initialized || input == NULL || output == NULL) {
        return -1;
    }
    /* Mock 模式：返回模拟识别结果，验证接口连通性 */
    printf("[ocr_engine MOCK] 输入图片: %s\n", input);
    snprintf(output, ENGINE_OUTPUT_MAX,
             "[MOCK OCR 结果] 已识别图片: \"%s\"\n"
             "第一行: EdgeSim 离线 OCR 测试\n"
             "第二行: (mock 桩，未启用真实推理)\n"
             "提示: 编译时定义 HAS_RKNN_LITE 并链接 librknnrt 可启用真实推理。",
             input);
    printf("[ocr_engine MOCK] 已生成模拟识别结果\n");
    return 0;
}

void ocr_engine_destroy(void)
{
    if (!g_mock_initialized) {
        return;
    }
    printf("[ocr_engine MOCK] 模拟释放资源\n");
    g_mock_initialized = 0;
}

#endif /* HAS_RKNN_LITE */
