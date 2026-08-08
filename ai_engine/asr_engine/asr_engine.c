/*
 * =============================================================================
 * EdgeSim ASR 引擎封装模块实现文件  asr_engine.c
 * =============================================================================
 * 【文件作用】
 *   封装 whisper.cpp C API，实现统一的 init/run/destroy 三接口。
 *   加载 INT8 量化的 Whisper 模型，将音频文件转写为文字。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.4 节
 *
 * 【whisper.cpp API 核心流程（新手重点）】
 *   1. whisper_init_from_file_with_params(path, params)  从文件加载模型
 *   2. whisper_full_default_params(strategy)             获取默认推理参数
 *   3. whisper_full(ctx, params, samples, n_samples)     执行完整推理
 *   4. whisper_full_n_segments(ctx)                      获取分段数
 *   5. whisper_full_get_segment_text(ctx, i)             取第 i 段文本
 *   6. whisper_free(ctx)                                 释放模型
 *
 * 【音频读取说明】
 *   whisper.cpp 不带音频解码功能，需要：
 *   - 推荐方案：使用 wav-reader（whisper.cpp 示例自带的 wav.h）
 *   - 通用方案：使用 miniaudio / dr_wav / stb_vorbis
 *   本封装使用最简单的 WAV 直读，仅支持 16kHz 单声道 PCM WAV
 *
 * 【条件编译】
 *   HAS_WHISPER_CPP 定义时：调用真实 whisper.cpp API
 *   未定义时：运行 mock 桩，返回模拟结果（便于 PC 环境测试接口）
 * =============================================================================
 */

#include "asr_engine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 输出缓冲区最大长度（与 llm_engine / ocr_engine 保持一致） */
#define ENGINE_OUTPUT_MAX 8192

/* 音频读取最大样本数（16kHz * 30秒 = 480000，足够覆盖语音提问） */
#define ASR_MAX_SAMPLES 480000

/* =========================================================================
 * 条件编译：真实 whisper.cpp 实现
 * ========================================================================= */
#ifdef HAS_WHISPER_CPP

/* 引入 whisper.cpp 主头文件（路径由 CMake 指定） */
#include "whisper.h"

/* ---- 模块内部状态（static，仅本文件可见）---- */
static struct whisper_context *g_whisper_ctx = NULL;  /* whisper 推理上下文 */
static int                     g_initialized = 0;     /* 初始化标志 */

/*
 * read_wav_file —— 内部辅助函数：读取 16kHz 单声道 PCM WAV
 * 【参数】path      ：WAV 文件路径
 * 【参数】samples   ：输出样本数组（调用方分配，float*）
 * 【参数】max_n     ：数组最大容量
 * 【返回值】实际读取的样本数，<0=失败
 * 【说明】仅支持最简单的 PCM WAV（format=1, channels=1, bits=16）
 *        复杂格式请改用 dr_wav 等第三方库
 * 【WAV 格式基础】
 *   RIFF 头 12 字节：'RIFF' + size + 'WAVE'
 *   fmt  子块 24 字节：'fmt ' + size + format + channels + sample_rate
 *                      + byte_rate + block_align + bits_per_sample
 *   data 子块 8 字节+数据：'data' + size + PCM 样本...
 */
static int read_wav_file(const char *path, float *samples, int max_n)
{
    FILE *fp = NULL;
    unsigned char header[44];   /* WAV 文件头 44 字节 */
    int sample_rate;
    short channels;
    short bits_per_sample;
    short format;
    int data_size = 0;
    int n_samples = 0;
    int i;

    /* 二进制只读打开 */
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("[asr_engine] 无法打开音频文件: %s\n", path);
        return -1;
    }

    /* 读取 44 字节标准 WAV 头 */
    if (fread(header, 1, 44, fp) != 44) {
        printf("[asr_engine] WAV 文件头读取失败\n");
        fclose(fp);
        return -1;
    }

    /* 校验 RIFF 标识 */
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        printf("[asr_engine] 非 WAV 格式（缺少 RIFF/WAVE 标识）\n");
        fclose(fp);
        return -1;
    }

    /* 解析 fmt 子块字段（小端序） */
    format           = header[20] | (header[21] << 8);
    channels         = header[22] | (header[23] << 8);
    sample_rate      = header[24] | (header[25] << 8)
                     | (header[26] << 16) | (header[27] << 24);
    bits_per_sample  = header[34] | (header[35] << 8);

    /* Whisper 要求 16kHz 单声道 16-bit PCM */
    if (format != 1 || channels != 1 || sample_rate != 16000
        || bits_per_sample != 16) {
        printf("[asr_engine] WAV 格式不支持：format=%d ch=%d rate=%d bits=%d\n"
               "  要求: PCM(1) / 单声道(1) / 16kHz / 16-bit\n",
               format, channels, sample_rate, bits_per_sample);
        fclose(fp);
        return -1;
    }

    /* data 子块大小（PCM 字节数） */
    data_size = header[40] | (header[41] << 8)
              | (header[42] << 16) | (header[43] << 24);
    n_samples = data_size / 2;   /* 16-bit = 2 字节/样本 */

    /* 防止超出缓冲区 */
    if (n_samples > max_n) {
        n_samples = max_n;
    }

    /* 逐样本读取 int16 并归一化到 float[-1, 1]
     * Whisper 接受 float 数组，幅值范围 -1 到 1 */
    for (i = 0; i < n_samples; i++) {
        short s;
        if (fread(&s, 2, 1, fp) != 1) {
            printf("[asr_engine] 样本读取中断于第 %d 个\n", i);
            break;
        }
        samples[i] = (float)s / 32768.0f;
    }
    fclose(fp);
    printf("[asr_engine] WAV 读取完成: %d 个样本（约 %.1f 秒）\n",
           i, (float)i / 16000.0f);
    return i;
}

/*
 * 接口 1：asr_engine_init —— 加载 Whisper 模型
 */
int asr_engine_init(const char *model_path)
{
    struct whisper_context_params cparams;

    if (model_path == NULL) {
        return -1;
    }
    if (g_initialized) {
        printf("[asr_engine] 已初始化，请先 destroy\n");
        return -1;
    }

    /* ---- 1. 获取默认上下文参数 ----
     * whisper_context_default_params 返回默认值，通常无需修改 */
    cparams = whisper_context_default_params();

    /* ---- 2. 从文件加载模型 ----
     * whisper_init_from_file_with_params 参数：
     *   model_path：模型文件路径（如 "ggml-base-int8.bin"）
     *   cparams   ：上下文参数
     * 返回 NULL 表示加载失败（路径错/文件损坏/内存不足） */
    g_whisper_ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (g_whisper_ctx == NULL) {
        printf("[asr_engine] 模型加载失败: %s\n", model_path);
        return -1;
    }

    g_initialized = 1;
    printf("[asr_engine] 模型加载成功: %s\n", model_path);
    return 0;
}

/*
 * 接口 2：asr_engine_run —— 执行语音转文字
 */
int asr_engine_run(const char *input, char *output)
{
    float *samples = NULL;          /* 音频样本数组（float 归一化） */
    int n_samples = 0;              /* 实际样本数 */
    int ret = 0;
    int n_segments, i;
    int offset = 0;
    struct whisper_full_params params;

    if (!g_initialized || input == NULL || output == NULL) {
        return -1;
    }

    /* ---- 1. 分配样本缓冲区 ---- */
    samples = (float *)malloc(ASR_MAX_SAMPLES * sizeof(float));
    if (samples == NULL) {
        printf("[asr_engine] 样本缓冲区分配失败\n");
        return -1;
    }

    /* ---- 2. 读取 WAV 文件 ----
     * 内部辅助函数：仅支持 16kHz 单声道 16-bit PCM */
    n_samples = read_wav_file(input, samples, ASR_MAX_SAMPLES);
    if (n_samples <= 0) {
        printf("[asr_engine] 音频读取失败\n");
        free(samples);
        return -1;
    }

    /* ---- 3. 配置推理参数 ----
     * whisper_full_default_params 参数：
     *   WHISPER_SAMPLING_GREEDY：贪心解码（速度快，适合短句）
     *   WHISPER_SAMPLING_BEAM_SEARCH：束搜索（质量高，速度慢）
     * 返回默认参数结构体，可按需修改字段 */
    params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    /* 关键参数说明：
     *   translate          ：false=保持原语言，true=翻译为英文
     *   language           ：语言代码，"zh"=中文，"en"=英文，NULL=自动检测
     *   n_threads          ：CPU 线程数
     *   no_context         ：true=不使用上一次的上下文（每次独立转写）
     *   single_segment     ：false=允许分段，true=强制单段输出
     *   initial_prompt     ：初始提示文本，引导模型输出简体中文
     *                        whisper 默认倾向繁体，给定简体中文 prompt 可纠正 */
    params.translate     = false;
    params.language      = "zh";       /* 中文场景，可改 "en" 或 NULL 自动检测 */
    params.n_threads     = 4;
    params.no_context    = true;
    params.single_segment = false;
    params.initial_prompt = "以下是简体中文的句子。";  /* 引导输出简体 */

    /* ---- 4. 执行完整推理 ----
     * whisper_full 参数：
     *   g_whisper_ctx ：上下文
     *   params        ：推理参数
     *   samples       ：音频样本数组（float, -1~1）
     *   n_samples     ：样本数
     * 返回 0=成功，<0=错误 */
    ret = whisper_full(g_whisper_ctx, params, samples, n_samples);
    free(samples);
    if (ret != 0) {
        printf("[asr_engine] whisper_full 失败: %d\n", ret);
        return -1;
    }
    printf("[asr_engine] 推理完成\n");

    /* ---- 5. 拼接所有分段的文本 ----
     * whisper 将长音频按语义分段，每段对应一段文字 */
    output[0] = '\0';
    n_segments = whisper_full_n_segments(g_whisper_ctx);
    printf("[asr_engine] 共 %d 个分段\n", n_segments);

    for (i = 0; i < n_segments; i++) {
        /* whisper_full_get_segment_text 返回第 i 段的文本（const char*）
         * 内存由 whisper 内部管理，无需释放，但调用者不得保存指针跨多次推理 */
        const char *seg_text = whisper_full_get_segment_text(g_whisper_ctx, i);
        if (seg_text == NULL) {
            continue;
        }
        int seg_len = (int)strlen(seg_text);
        /* 拼接到输出缓冲区，注意防溢出 */
        if (offset + seg_len + 2 < ENGINE_OUTPUT_MAX) {
            strcat(output, seg_text);
            offset += seg_len;
            /* 段间加换行 */
            if (i < n_segments - 1) {
                strcat(output, "\n");
                offset += 1;
            }
        } else {
            /* 输出缓冲区已满，截断 */
            printf("[asr_engine] 输出缓冲区已满，截断\n");
            break;
        }
    }

    printf("[asr_engine] 转写完成，输出 %d 字节\n", offset);
    printf("[asr_engine] 转写结果: %s\n", output);
    return 0;
}

/*
 * 接口 3：asr_engine_destroy —— 销毁引擎
 */
void asr_engine_destroy(void)
{
    if (!g_initialized) {
        return;
    }

    /* whisper_free 释放上下文与所有内部资源（模型权重、KV缓存等） */
    whisper_free(g_whisper_ctx);
    g_whisper_ctx = NULL;

    g_initialized = 0;
    printf("[asr_engine] 资源已释放\n");
}


/* =========================================================================
 * 条件编译：Mock 桩实现（无 whisper.cpp 时编译，PC 环境使用）
 * ========================================================================= */
#else  /* !HAS_WHISPER_CPP */

static int g_mock_initialized = 0;

int asr_engine_init(const char *model_path)
{
    if (model_path == NULL) {
        return -1;
    }
    if (g_mock_initialized) {
        return -1;
    }
    /* Mock 模式：不真正加载模型，仅打印提示 */
    printf("[asr_engine MOCK] 模拟加载 Whisper 模型: %s\n", model_path);
    printf("[asr_engine MOCK] (未定义 HAS_WHISPER_CPP，使用 mock 桩)\n");
    g_mock_initialized = 1;
    return 0;
}

int asr_engine_run(const char *input, char *output)
{
    if (!g_mock_initialized || input == NULL || output == NULL) {
        return -1;
    }
    /* Mock 模式：返回模拟转写结果，验证接口连通性 */
    printf("[asr_engine MOCK] 输入音频: %s\n", input);
    snprintf(output, ENGINE_OUTPUT_MAX,
             "[MOCK ASR 结果] 已转写音频: \"%s\"\n"
             "识别内容: 你好，这是 EdgeSim 离线语音转写测试。\n"
             "提示: 编译时定义 HAS_WHISPER_CPP 并链接 whisper.cpp 可启用真实推理。",
             input);
    printf("[asr_engine MOCK] 已生成模拟转写结果\n");
    return 0;
}

void asr_engine_destroy(void)
{
    if (!g_mock_initialized) {
        return;
    }
    printf("[asr_engine MOCK] 模拟释放资源\n");
    g_mock_initialized = 0;
}

#endif /* HAS_WHISPER_CPP */
