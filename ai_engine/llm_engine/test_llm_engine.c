/*
 * =============================================================================
 * EdgeSim LLM 引擎独立测试  test_llm_engine.c
 * =============================================================================
 * 【测试流程】
 *   1. init：加载模型（mock 或真实）
 *   2. run：发送问题，接收回复
 *   3. destroy：释放资源
 * =============================================================================
 */
#include <stdio.h>
#include <string.h>
#include "llm_engine.h"

int main(void)
{
    char output[8192];   /* 输出缓冲区（至少 ENGINE_OUTPUT_MAX） */
    int ret;

    printf("============================================================\n");
    printf("  EdgeSim llm_engine 模块独立测试\n");
    printf("============================================================\n\n");

    /* 步骤 1：初始化 */
    printf("【步骤 1】llm_engine_init\n");
    ret = llm_engine_init("qwen1.8b-int8.gguf");
    if (ret != 0) {
        printf("  初始化失败\n");
        return 1;
    }
    printf("  初始化成功\n\n");

    /* 步骤 2：多轮推理测试 */
    printf("【步骤 2】llm_engine_run 多轮推理\n\n");

    /* 第一轮 */
    printf("---- 第一轮 ----\n");
    ret = llm_engine_run("你好，请用一句话介绍自己。", output);
    if (ret == 0) {
        printf("  回复: %s\n\n", output);
    }

    /* 第二轮 */
    printf("---- 第二轮 ----\n");
    ret = llm_engine_run("翻译：Hello World", output);
    if (ret == 0) {
        printf("  回复: %s\n\n", output);
    }

    /* 第三轮（长文本） */
    printf("---- 第三轮 ----\n");
    ret = llm_engine_run("请写一段关于嵌入式AI的简短笔记。", output);
    if (ret == 0) {
        printf("  回复: %s\n\n", output);
    }

    /* 步骤 3：销毁 */
    printf("【步骤 3】llm_engine_destroy\n");
    llm_engine_destroy();
    printf("  销毁成功\n");

    printf("\n============================================================\n");
    printf("  测试完成\n");
    printf("============================================================\n");
    return 0;
}
