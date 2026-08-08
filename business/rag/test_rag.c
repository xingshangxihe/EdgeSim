/*
 * =============================================================================
 * EdgeSim rag 模块测试程序  test_rag.c
 * =============================================================================
 * 【测试内容】
 *   1. 在 /tmp 创建临时知识库 TXT 文件（含英文 + 中文内容）
 *   2. db_init(":memory:") 初始化内存数据库
 *   3. kb_add 把文件登记进 kb_files 表
 *   4. rag_retrieve 检索：
 *      a. 英文查询 "edge computing" 应命中英文段落
 *      b. 中文查询 "边缘计算" 应命中中文段落
 *   5. 打印结果并输出 PASS/FAIL
 *
 * 【编译运行】
 *   由 CMake BUILD_TESTS=ON 自动构建，产物 build/bin/test_rag
 * =============================================================================
 */

#include "rag.h"
#include "sqlite_db.h"

#include <stdio.h>
#include <string.h>

/* 临时知识库文件路径 */
#define TEST_KB_PATH "/tmp/rag_test_kb.txt"


/* -----------------------------------------------------------------------------
 * main —— 测试入口
 * -----------------------------------------------------------------------------
 */
int main(void)
{
    FILE *fp;
    char  result[RAG_CONTEXT_MAX];
    int   ok_en = 0;
    int   ok_zh = 0;

    /* ---- 1. 创建临时知识库文件（含英文与中文内容） ---- */
    fp = fopen(TEST_KB_PATH, "w");
    if (fp == NULL) {
        printf("[test_rag] FAIL: 无法创建临时文件 %s\n", TEST_KB_PATH);
        return 1;
    }
    fputs("EdgeSim is an offline AI client with embedded hardware simulation.\n"
          "Edge computing processes data near the source instead of the cloud.\n"
          "边缘计算是一种将数据处理放在靠近数据源位置的技术。\n"
          "EdgeSim 通过内存仿真器模拟低内存开发板环境。\n", fp);
    fclose(fp);
    printf("[test_rag] 已创建临时知识库: %s\n", TEST_KB_PATH);

    /* ---- 2. 初始化内存数据库 ---- */
    if (db_init(":memory:") != 0) {
        printf("[test_rag] FAIL: db_init 失败\n");
        return 1;
    }
    printf("[test_rag] 数据库已初始化（内存库）\n");

    /* ---- 3. 登记文件进 kb_files 表（type=0 表示 TXT） ---- */
    if (kb_add(TEST_KB_PATH, 0) < 0) {
        printf("[test_rag] FAIL: kb_add 失败\n");
        db_close();
        return 1;
    }
    printf("[test_rag] 文件已入库 kb_files\n");

    /* ---- 4a. 英文检索 ---- */
    memset(result, 0, sizeof(result));
    if (rag_retrieve("edge computing", result, sizeof(result), 2) != 0) {
        printf("[test_rag] FAIL: rag_retrieve(英文) 返回失败\n");
        db_close();
        return 1;
    }
    printf("[test_rag] 英文检索结果:\n%s\n", result);
    ok_en = (strstr(result, "edge computing") != NULL ||
            strstr(result, "Edge computing") != NULL);

    /* ---- 4b. 中文检索 ---- */
    memset(result, 0, sizeof(result));
    if (rag_retrieve("边缘计算", result, sizeof(result), 2) != 0) {
        printf("[test_rag] FAIL: rag_retrieve(中文) 返回失败\n");
        db_close();
        return 1;
    }
    printf("[test_rag] 中文检索结果:\n%s\n", result);
    ok_zh = (strstr(result, "边缘计算") != NULL);

    db_close();

    /* ---- 5. 汇总 ---- */
    printf("======================================\n");
    printf("  英文检索: %s\n", ok_en ? "PASS" : "FAIL");
    printf("  中文检索: %s\n", ok_zh ? "PASS" : "FAIL");
    printf("======================================\n");

    return (ok_en && ok_zh) ? 0 : 1;
}
