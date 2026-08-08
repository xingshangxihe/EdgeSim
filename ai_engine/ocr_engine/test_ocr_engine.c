/*
 * =============================================================================
 * EdgeSim OCR 引擎独立测试  test_ocr_engine.c
 * =============================================================================
 * 【测试流程】
 *   1. init：加载 RKNN 模型（mock 或真实）
 *   2. run：传入图片路径，接收识别文字
 *   3. destroy：释放资源
 * =============================================================================
 */
#include <stdio.h>
#include <string.h>
#include "ocr_engine.h"

int main(void)
{
    char output[8192];   /* 输出缓冲区（至少 ENGINE_OUTPUT_MAX） */
    int ret;

    printf("============================================================\n");
    printf("  EdgeSim ocr_engine 模块独立测试\n");
    printf("============================================================\n\n");

    /* 步骤 1：初始化 */
    printf("【步骤 1】ocr_engine_init\n");
    ret = ocr_engine_init("paddleocr-int8.rknn");
    if (ret != 0) {
        printf("  初始化失败\n");
        return 1;
    }
    printf("  初始化成功\n\n");

    /* 步骤 2：多张图片识别测试 */
    printf("【步骤 2】ocr_engine_run 多图识别\n\n");

    /* 第一张：截图 */
    printf("---- 第一张 ----\n");
    ret = ocr_engine_run("screenshot_01.png", output);
    if (ret == 0) {
        printf("  识别结果:\n%s\n\n", output);
    }

    /* 第二张：手写笔记 */
    printf("---- 第二张 ----\n");
    ret = ocr_engine_run("note_handwriting.jpg", output);
    if (ret == 0) {
        printf("  识别结果:\n%s\n\n", output);
    }

    /* 第三张：照片 */
    printf("---- 第三张 ----\n");
    ret = ocr_engine_run("photo_doc.bmp", output);
    if (ret == 0) {
        printf("  识别结果:\n%s\n\n", output);
    }

    /* 步骤 3：销毁 */
    printf("【步骤 3】ocr_engine_destroy\n");
    ocr_engine_destroy();
    printf("  销毁成功\n");

    printf("\n============================================================\n");
    printf("  测试完成\n");
    printf("============================================================\n");
    return 0;
}
