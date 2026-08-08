/*
 * =============================================================================
 * EdgeSim SQLite 数据库封装模块独立测试  test_sqlite_db.c
 * =============================================================================
 * 【文件作用】
 *   独立测试 sqlite_db 模块的三表 CRUD（增删改查）接口。
 *   使用 ":memory:" 内存数据库，测试完不残留文件。
 *
 * 【测试流程】
 *   步骤 1：db_init 初始化（内存数据库）
 *   步骤 2：chat_history 增删改查测试
 *   步骤 3：kb_files 增删改查测试
 *   步骤 4：perf_log 增删改查测试
 *   步骤 5：db_close 关闭
 *
 * 【编译运行】
 *   make test          # 一键编译并运行
 *   或手动：
 *   gcc -Wall -g test_sqlite_db.c sqlite_db.c -o test_sqlite_db -lsqlite3
 *   ./test_sqlite_db
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.3 节 + 第 1.4.7 条「完成数据持久化测试」
 * =============================================================================
 */

#include <stdio.h>
#include <string.h>
#include "sqlite_db.h"


/* =============================================================================
 * 辅助函数：打印分隔线
 * =============================================================================
 */
static void print_sep(const char *title)
{
    printf("\n------------------------------------------------------------\n");
    printf("  %s\n", title);
    printf("------------------------------------------------------------\n");
}


/* =============================================================================
 * 步骤 2：chat_history 表 CRUD 测试
 * =============================================================================
 */
static void test_chat_crud(void)
{
    chat_record_t records[100];   /* 查询结果数组 */
    int actual_count = 0;         /* 实际查询条数 */
    int id1, id2;                 /* 插入返回的 ID */
    int i;

    print_sep("步骤 2: chat_history 增删改查测试");

    /* ---- 增：插入 2 条对话记录 ----
     * 模拟一轮对话：用户提问 + AI 回答
     * role: 0=用户, 1=AI */
    printf("\n[增] 插入对话记录...\n");
    id1 = chat_save(NULL, 0, "你好，请帮我翻译这段话");
    printf("  插入用户消息，id=%d\n", id1);

    id2 = chat_save(NULL, 1, "好的，请提供需要翻译的文本。");
    printf("  插入AI回复，id=%d\n", id2);

    /* 测试含特殊字符的内容（验证 SQL 注入防护）
     * 内容含单引号，若用字符串拼接会导致 SQL 语法错误。
     * 用 prepare+bind 则自动转义，安全插入。 */
    int id3 = chat_save(NULL, 0, "It's a test with 'single quotes' and \"double\"");
    printf("  插入含特殊字符的消息，id=%d\n", id3);

    /* ---- 查：查询所有对话记录 ---- */
    printf("\n[查] 查询所有对话记录...\n");
    chat_query(records, 100, &actual_count);
    printf("  共 %d 条记录：\n", actual_count);
    for (i = 0; i < actual_count; i++) {
        printf("    id=%d  time=%s  role=%s  content=%s\n",
               records[i].id,
               records[i].time,
               records[i].role == 0 ? "用户" : "AI",
               records[i].content);
    }

    /* ---- 改：更新 id2 的内容 ---- */
    printf("\n[改] 更新 id=%d 的内容...\n", id2);
    chat_update(id2, 1, "这是更新后的AI回复内容。");
    /* 验证更新结果 */
    chat_query(records, 100, &actual_count);
    for (i = 0; i < actual_count; i++) {
        if (records[i].id == id2) {
            printf("  更新后: content=%s\n", records[i].content);
        }
    }

    /* ---- 删：删除 id3 ---- */
    printf("\n[删] 删除 id=%d...\n", id3);
    chat_delete(id3);
    /* 验证删除 */
    chat_query(records, 100, &actual_count);
    printf("  删除后剩余 %d 条记录\n", actual_count);

    /* ---- 清空 ---- */
    printf("\n[清空] 清空 chat_history...\n");
    chat_clear();
    chat_query(records, 100, &actual_count);
    printf("  清空后剩余 %d 条记录\n", actual_count);
}


/* =============================================================================
 * 步骤 3：kb_files 表 CRUD 测试
 * =============================================================================
 */
static void test_kb_crud(void)
{
    kb_file_t files[100];
    int actual_count = 0;
    int id1, id2, id3;
    int i;

    print_sep("步骤 3: kb_files 增删改查测试");

    /* ---- 增：导入 3 个知识库文件 ----
     * type: 0=txt 1=md 2=pdf 3=image */
    printf("\n[增] 导入知识库文件...\n");
    id1 = kb_add("/data/kb/notes.txt", 0);
    id2 = kb_add("/data/kb/manual.md", 1);
    id3 = kb_add("/data/kb/report.pdf", 2);
    printf("  导入 txt, id=%d\n", id1);
    printf("  导入 md,  id=%d\n", id2);
    printf("  导入 pdf, id=%d\n", id3);

    /* ---- 查 ---- */
    printf("\n[查] 查询知识库文件...\n");
    kb_query(files, 100, &actual_count);
    printf("  共 %d 个文件：\n", actual_count);
    for (i = 0; i < actual_count; i++) {
        const char *type_str[] = {"txt", "md", "pdf", "image"};
        printf("    id=%d  path=%s  type=%s  time=%s\n",
               files[i].id,
               files[i].path,
               type_str[files[i].type],
               files[i].create_time);
    }

    /* ---- 改 ---- */
    printf("\n[改] 更新 id=%d 的路径...\n", id1);
    kb_update(id1, "/data/kb/notes_v2.txt", 0);
    kb_query(files, 100, &actual_count);
    for (i = 0; i < actual_count; i++) {
        if (files[i].id == id1) {
            printf("  更新后: path=%s\n", files[i].path);
        }
    }

    /* ---- 删 ---- */
    printf("\n[删] 删除 id=%d...\n", id2);
    kb_delete(id2);
    kb_query(files, 100, &actual_count);
    printf("  删除后剩余 %d 个文件\n", actual_count);

    /* ---- 清空 ---- */
    printf("\n[清空] 清空 kb_files...\n");
    kb_clear();
    kb_query(files, 100, &actual_count);
    printf("  清空后剩余 %d 个文件\n", actual_count);
}


/* =============================================================================
 * 步骤 4：perf_log 表 CRUD 测试
 * =============================================================================
 */
static void test_perf_log_crud(void)
{
    perf_log_t logs[100];
    int actual_count = 0;
    int id1, id2, id3;
    int i;

    print_sep("步骤 4: perf_log 增删改查测试");

    /* ---- 增：记录 3 次推理性能 ----
     * model: 0=LLM 1=OCR 2=ASR */
    printf("\n[增] 记录性能日志...\n");
    id1 = perf_log_add(0, 1850.5, 180);   /* LLM: 1850.5ms, 180MB */
    id2 = perf_log_add(1, 820.3, 60);     /* OCR: 820.3ms, 60MB */
    id3 = perf_log_add(2, 2950.7, 90);    /* ASR: 2950.7ms, 90MB */
    printf("  LLM 日志, id=%d\n", id1);
    printf("  OCR 日志, id=%d\n", id2);
    printf("  ASR 日志, id=%d\n", id3);

    /* ---- 查 ---- */
    printf("\n[查] 查询性能日志...\n");
    perf_log_query(logs, 100, &actual_count);
    printf("  共 %d 条日志：\n", actual_count);
    for (i = 0; i < actual_count; i++) {
        const char *model_str[] = {"LLM", "OCR", "ASR"};
        printf("    id=%d  time=%s  model=%s  latency=%.2fms  mem_peak=%zuMB\n",
               logs[i].id,
               logs[i].time,
               model_str[logs[i].model],
               logs[i].latency,
               logs[i].mem_peak);
    }

    /* ---- 改 ---- */
    printf("\n[改] 更新 id=%d 的延迟和内存...\n", id1);
    perf_log_update(id1, 0, 1720.8, 175);  /* LLM 优化后：1720.8ms, 175MB */
    perf_log_query(logs, 100, &actual_count);
    for (i = 0; i < actual_count; i++) {
        if (logs[i].id == id1) {
            printf("  更新后: latency=%.2fms mem_peak=%zuMB\n",
                   logs[i].latency, logs[i].mem_peak);
        }
    }

    /* ---- 删 ---- */
    printf("\n[删] 删除 id=%d...\n", id3);
    perf_log_delete(id3);
    perf_log_query(logs, 100, &actual_count);
    printf("  删除后剩余 %d 条日志\n", actual_count);

    /* ---- 清空 ---- */
    printf("\n[清空] 清空 perf_log...\n");
    perf_log_clear();
    perf_log_query(logs, 100, &actual_count);
    printf("  清空后剩余 %d 条日志\n", actual_count);
}


/* =============================================================================
 * 主函数
 * =============================================================================
 */
int main(void)
{
    int ret;

    printf("============================================================\n");
    printf("  EdgeSim sqlite_db 模块独立测试\n");
    printf("============================================================\n");

    /* ---- 步骤 1：初始化数据库 ----
     * 使用 ":memory:" 创建内存数据库：
     *   - 不落盘，进程退出即销毁，适合测试
     *   - 正式使用时传文件路径如 "edgesim.db" */
    print_sep("步骤 1: db_init 初始化（内存数据库）");
    ret = db_init(":memory:");
    if (ret != 0) {
        printf("  初始化失败\n");
        return 1;
    }
    printf("  初始化成功\n");

    /* ---- 步骤 2~4：三表 CRUD 测试 ---- */
    test_chat_crud();
    test_kb_crud();
    test_perf_log_crud();

    /* ---- 步骤 5：关闭数据库 ---- */
    print_sep("步骤 5: db_close 关闭数据库");
    db_close();

    printf("\n============================================================\n");
    printf("  测试全部完成\n");
    printf("============================================================\n");

    return 0;
}
