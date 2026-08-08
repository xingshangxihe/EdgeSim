/*
 * =============================================================================
 * EdgeSim ASR 引擎独立测试  test_asr_engine.c
 * =============================================================================
 * 【测试流程】
 *   1. init：加载 Whisper 模型（mock 或真实）
 *   2. run：传入音频路径，接收转写文字
 *   3. destroy：释放资源
 * =============================================================================
 */
#include <stdio.h>
#include <string.h>
#include "asr_engine.h"

int main(void)
{
    char output[8192];   /* 输出缓冲区（至少 ENGINE_OUTPUT_MAX） */
    int ret;

    printf("============================================================\n");
    printf("  EdgeSim asr_engine 模块独立测试\n");
    printf("============================================================\n\n");

    /* 步骤 1：初始化 */
    printf("【步骤 1】asr_engine_init\n");
    ret = asr_engine_init("ggml-base-int8.bin");
    if (ret != 0) {
        printf("  初始化失败\n");
        return 1;
    }
    printf("  初始化成功\n\n");

    /* 步骤 2：多段音频识别测试 */
    printf("【步骤 2】asr_engine_run 多段音频识别\n\n");

    /* 第一段：语音提问 */
    printf("---- 第一段 ----\n");
    ret = asr_engine_run("voice_question_01.wav", output);
    if (ret == 0) {
        printf("  转写结果:\n%s\n\n", output);
    }

    /* 第二段：语音备忘录 */
    printf("---- 第二段 ----\n");
    ret = asr_engine_run("voice_memo_02.wav", output);
    if (ret == 0) {
        printf("  转写结果:\n%s\n\n", output);
    }

    /* 第三段：语音翻译 */
    printf("---- 第三段 ----\n");
    ret = asr_engine_run("voice_translate_03.wav", output);
    if (ret == 0) {
        printf("  转写结果:\n%s\n\n", output);
    }

    /* 步骤 3：销毁 */
    printf("【步骤 3】asr_engine_destroy\n");
    asr_engine_destroy();
    printf("  销毁成功\n");

    printf("\n============================================================\n");
    printf("  测试完成\n");
    printf("============================================================\n");
    return 0;
}
