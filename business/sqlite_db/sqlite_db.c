/*
 * =============================================================================
 * EdgeSim SQLite 数据库封装模块实现文件  sqlite_db.c
 * =============================================================================
 * 【文件作用】
 *   实现 sqlite_db.h 中声明的全部接口。封装 SQLite3 C API，
 *   提供对话历史、知识库文件、性能日志三张表的增删改查。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.3 节
 *
 * 【SQLite API 核心流程（新手重点）】
 *   1. sqlite3_open(path, &db)              打开数据库
 *   2. sqlite3_exec(db, sql, cb, arg, &err) 执行简单 SQL（建表等）
 *   3. sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)  预编译 SQL 语句
 *   4. sqlite3_bind_*(stmt, idx, val)       绑定参数（防 SQL 注入）
 *   5. sqlite3_step(stmt)                   执行（SQLITE_ROW=有数据 SQLITE_DONE=完成）
 *   6. sqlite3_column_*(stmt, col)          读取结果列
 *   7. sqlite3_finalize(stmt)               释放预编译语句
 *   8. sqlite3_close(db)                    关闭数据库
 *
 * 【SQL 注入防护】
 *   所有含用户数据的 SQL 都用 prepare + bind，绝不用字符串拼接。
 *   bind 会自动转义特殊字符（如单引号），防止注入攻击。
 * =============================================================================
 */


/* =============================================================================
 * 一、头文件包含区
 * ============================================================================= */
#include "sqlite_db.h"   /* 本模块公共接口 */

#include <stdio.h>       /* printf/sprintf */
#include <stdlib.h>      /* malloc/free */
#include <string.h>      /* memset/strncpy/strlen */
#include <time.h>        /* time/localtime/strftime */

#include <sqlite3.h>     /* SQLite3 C API（需安装 libsqlite3-dev）*/


/* =============================================================================
 * 二、模块内部全局变量
 * =============================================================================
 */
/* 数据库连接句柄。SQLite 所有操作都通过此句柄进行。
 * 声明为 static，仅本文件可见，外部通过接口间接操作。 */
static sqlite3 *g_db = NULL;

/* 初始化标志 */
static int g_initialized = 0;


/* =============================================================================
 * 三、内部辅助函数（static）
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：get_current_time_str
 * -----------------------------------------------------------------------------
 * 【作用】获取当前时间字符串，格式 "YYYY-MM-DD HH:MM:SS"
 * 【参数】buf：输出缓冲区
 * 【参数】size：缓冲区大小
 * 【说明】多处接口需要记录时间，抽取为公共函数。
 * -----------------------------------------------------------------------------
 */
static void get_current_time_str(char *buf, int size)
{
    time_t now;                /* Unix 时间戳 */
    struct tm tm_info;         /* 分解时间结构体 */

    /* 获取当前时间 */
    now = time(NULL);

    /* localtime_r 是线程安全版本（localtime 返回静态变量，多线程不安全）
     * 将 time_t 转换为 struct tm（年月日时分秒） */
    localtime_r(&now, &tm_info);

    /* 格式化时间字符串。
     * %Y=年 %m=月 %d=日 %H=时 %M=分 %S=秒 */
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_info);
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：create_tables
 * -----------------------------------------------------------------------------
 * 【作用】创建三张表（如果不存在）
 * 【SQL 语法说明】
 *   CREATE TABLE IF NOT EXISTS 表名 (
 *     列名 类型 约束,
 *     ...
 *   );
 *   IF NOT EXISTS：表已存在则不报错（幂等操作）
 *
 * 【SQLite 数据类型】
 *   INTEGER  ：整数（SQLite 按值大小自动选 1/2/4/8 字节）
 *   TEXT     ：文本字符串（UTF-8）
 *   REAL     ：浮点数（8 字节 double）
 *   BLOB     ：二进制数据
 *   PRIMARY KEY AUTOINCREMENT：自增主键
 *
 * 【sqlite3_exec 说明】
 *   sqlite3_exec 是简化版执行函数，适用于不需要返回结果的 SQL（建表/删表等）。
 *   参数：db, sql, 回调函数, 回调参数, 错误信息指针
 *   返回 SQLITE_OK=成功
 * -----------------------------------------------------------------------------
 */
static int create_tables(void)
{
    int ret;                    /* SQLite 返回值 */
    char *errmsg = NULL;        /* 错误信息 */

    /* ---- 创建 chat_history 表 ----
     * id        INTEGER PRIMARY KEY AUTOINCREMENT : 自增主键
     * time      TEXT NOT NULL                     : 时间，非空
     * role      INTEGER DEFAULT 0                 : 角色，默认 0(用户)
     * content   TEXT NOT NULL                     : 内容，非空 */
    const char *sql_chat =
        "CREATE TABLE IF NOT EXISTS chat_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "  time TEXT NOT NULL, "
        "  role INTEGER DEFAULT 0, "
        "  content TEXT NOT NULL"
        ");";

    /* ---- 创建 kb_files 表 ----
     * id          INTEGER PRIMARY KEY AUTOINCREMENT : 自增主键
     * path        TEXT NOT NULL                     : 文件路径
     * type        INTEGER DEFAULT 0                 : 文件类型
     * create_time TEXT NOT NULL                     : 导入时间 */
    const char *sql_kb =
        "CREATE TABLE IF NOT EXISTS kb_files ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "  path TEXT NOT NULL, "
        "  type INTEGER DEFAULT 0, "
        "  create_time TEXT NOT NULL"
        ");";

    /* ---- 创建 perf_log 表 ----
     * id        INTEGER PRIMARY KEY AUTOINCREMENT : 自增主键
     * time      TEXT NOT NULL                     : 记录时间
     * model     INTEGER DEFAULT 0                 : 模型 ID
     * latency   REAL DEFAULT 0                    : 延迟(毫秒)
     * mem_peak  INTEGER DEFAULT 0                 : 内存峰值(MB) */
    const char *sql_perf =
        "CREATE TABLE IF NOT EXISTS perf_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "  time TEXT NOT NULL, "
        "  model INTEGER DEFAULT 0, "
        "  latency REAL DEFAULT 0, "
        "  mem_peak INTEGER DEFAULT 0"
        ");";

    /* 执行建表 SQL。
     * sqlite3_exec 参数：
     *   db      ：数据库句柄
     *   sql     ：SQL 语句字符串
     *   callback：查询回调函数（建表无结果，传 NULL）
     *   arg     ：回调参数（NULL）
     *   errmsg  ：错误信息输出（失败时指向错误字符串，需 sqlite3_free 释放） */
    ret = sqlite3_exec(g_db, sql_chat, NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] 创建 chat_history 失败: %s\n", errmsg);
        sqlite3_free(errmsg);   /* SQLite 分配的内存必须用 sqlite3_free 释放 */
        return -1;
    }

    ret = sqlite3_exec(g_db, sql_kb, NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] 创建 kb_files 失败: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    ret = sqlite3_exec(g_db, sql_perf, NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] 创建 perf_log 失败: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    printf("[sqlite_db] 三张表创建成功\n");
    return 0;
}


/* =============================================================================
 * 四、数据库生命周期接口实现
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 接口：db_init —— 打开数据库并建表
 * -----------------------------------------------------------------------------
 * 【参数】db_path：数据库文件路径
 *         ":memory:" = 内存数据库（测试用，进程退出销毁）
 *         "edgesim.db" = 文件数据库（持久化）
 * 【返回值】0=成功，-1=失败
 * 【SQLite API】
 *   sqlite3_open(path, &db)：
 *     打开数据库，不存在则创建。
 *     返回 SQLITE_OK=成功，其他=失败。
 *     失败时 db 可能仍被分配（需 sqlite3_close 释放）。
 * -----------------------------------------------------------------------------
 */
int db_init(const char *db_path)
{
    int ret;   /* SQLite 返回值 */

    /* 防止重复初始化 */
    if (g_initialized) {
        printf("[sqlite_db] 已初始化，请先 db_close()\n");
        return -1;
    }
    if (db_path == NULL) {
        return -1;
    }

    /* 打开数据库。
     * sqlite3_open 第一个参数是路径，第二个是 sqlite3* 指针的地址。
     * 路径为 ":memory:" 时创建纯内存数据库，不落盘，适合测试。 */
    ret = sqlite3_open(db_path, &g_db);
    if (ret != SQLITE_OK) {
        /* sqlite3_errmsg 返回错误描述字符串（不需要 free） */
        printf("[sqlite_db] 打开数据库失败: %s\n", sqlite3_errmsg(g_db));
        /* 即使打开失败也要 close（释放 sqlite3 结构体） */
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    printf("[sqlite_db] 数据库已打开: %s\n", db_path);

    /* 创建表 */
    if (create_tables() < 0) {
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    g_initialized = 1;
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口：db_close —— 关闭数据库
 * -----------------------------------------------------------------------------
 * 【返回值】0=成功，-1=失败
 * 【SQLite API】
 *   sqlite3_close(db)：
 *     关闭数据库连接，释放资源。
 *     若有未 finalize 的 stmt，会返回 SQLITE_BUSY，所以务必先 finalize。
 * -----------------------------------------------------------------------------
 */
int db_close(void)
{
    int ret;

    if (!g_initialized) {
        return -1;
    }

    /* 关闭数据库 */
    ret = sqlite3_close(g_db);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] 关闭数据库失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    g_db = NULL;
    g_initialized = 0;
    printf("[sqlite_db] 数据库已关闭\n");
    return 0;
}


/* =============================================================================
 * 五、chat_history 表 CRUD 实现
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * chat_save —— 插入对话记录（增）
 * -----------------------------------------------------------------------------
 * 【SQL】INSERT INTO chat_history (time, role, content) VALUES (?, ?, ?)
 * 【参数绑定】
 *   ?1 = time_str（或当前时间）
 *   ?2 = role
 *   ?3 = content
 * 【SQL 注入防护】
 *   用 ? 占位符 + sqlite3_bind_text，不拼接字符串。
 *   即使 content 含单引号（如 "it's"），也不会被解释为 SQL 结束。
 * -----------------------------------------------------------------------------
 */
int chat_save(const char *time_str, int role, const char *content)
{
    sqlite3_stmt *stmt = NULL;    /* 预编译语句句柄 */
    int ret;                      /* SQLite 返回值 */
    char time_buf[32];            /* 时间缓冲区 */
    int new_id;                   /* 新插入记录的 ID */

    if (!g_initialized || content == NULL) {
        return -1;
    }

    /* 处理时间参数：NULL 则取当前时间 */
    if (time_str == NULL) {
        get_current_time_str(time_buf, sizeof(time_buf));
        time_str = time_buf;
    }

    /* ---- 预编译 SQL ----
     * sqlite3_prepare_v2 参数：
     *   db      ：数据库句柄
     *   sql     ：SQL 语句（带 ? 占位符）
     *   -1      ：SQL 字符串长度，-1 表示自动计算到 '\0'
     *   &stmt   ：输出预编译语句句柄
     *   NULL    ：指向 sql 中未处理部分的指针（不需要，传 NULL） */
    const char *sql = "INSERT INTO chat_history (time, role, content) VALUES (?, ?, ?);";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] chat_save prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    /* ---- 绑定参数 ----
     * sqlite3_bind_text 参数：
     *   stmt    ：预编译语句
     *   1       ：参数索引（从 1 开始，对应第一个 ?）
     *   time_str：参数值
     *   -1      ：字符串长度，-1 表示自动计算到 '\0'
     *   SQLITE_STATIC ：SQLite 不复制数据，调用者保证在 step 前不被释放。
     *                   SQLITE_TRANSIENT 则让 SQLite 内部复制一份（更安全）。 */
    sqlite3_bind_text(stmt, 1, time_str, -1, SQLITE_STATIC);
    /* sqlite3_bind_int：绑定整数参数 */
    sqlite3_bind_int(stmt, 2, role);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);

    /* ---- 执行 ----
     * sqlite3_step 对 INSERT 返回 SQLITE_DONE 表示成功。
     * 返回 SQLITE_ROW 表示有数据可读（SELECT 时）。 */
    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] chat_save step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);   /* 出错也要 finalize 释放资源 */
        return -1;
    }

    /* 获取新插入记录的自增 ID。
     * sqlite3_last_insert_rowid 返回最近一次 INSERT 的 rowid。 */
    new_id = (int)sqlite3_last_insert_rowid(g_db);

    /* 释放预编译语句（必须调用，否则资源泄漏） */
    sqlite3_finalize(stmt);

    return new_id;
}


/*
 * -----------------------------------------------------------------------------
 * chat_query —— 查询对话记录（查）
 * -----------------------------------------------------------------------------
 * 【SQL】SELECT id, time, role, content FROM chat_history ORDER BY id
 * 【执行流程】
 *   1. prepare 预编译
 *   2. 循环 step，每次返回一行（SQLITE_ROW）
 *   3. 用 sqlite3_column_* 读取各列
 *   4. step 返回 SQLITE_DONE 表示遍历完毕
 * -----------------------------------------------------------------------------
 */
int chat_query(chat_record_t *out, int max_count, int *actual_count)
{
    sqlite3_stmt *stmt = NULL;
    int ret;
    int count = 0;               /* 已读取的记录数 */

    if (!g_initialized || out == NULL || actual_count == NULL) {
        return -1;
    }

    const char *sql = "SELECT id, time, role, content FROM chat_history ORDER BY id;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] chat_query prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    /* 循环读取每一行。
     * sqlite3_step 返回值：
     *   SQLITE_ROW ：有数据可读，可调 sqlite3_column_*
     *   SQLITE_DONE：遍历结束
     *   其他       ：错误 */
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        /* sqlite3_column_int(stmt, col_index)：读取整数列
         * sqlite3_column_text(stmt, col_index)：读取文本列（返回 const unsigned char*）
         *   注意返回类型是 unsigned char*，需强转为 char* */
        out[count].id = sqlite3_column_int(stmt, 0);

        /* 读取 time 列。sqlite3_column_text 返回指针，指向 SQLite 内部缓冲区，
         * 在下次 step/finalize 前有效，所以需要 strncpy 复制到我们的结构体。 */
        const char *time_val = (const char *)sqlite3_column_text(stmt, 1);
        strncpy(out[count].time, time_val, sizeof(out[count].time) - 1);
        out[count].time[sizeof(out[count].time) - 1] = '\0';  /* 确保 '\0' 结尾 */

        out[count].role = sqlite3_column_int(stmt, 2);

        const char *content_val = (const char *)sqlite3_column_text(stmt, 3);
        strncpy(out[count].content, content_val, sizeof(out[count].content) - 1);
        out[count].content[sizeof(out[count].content) - 1] = '\0';

        count++;
    }

    *actual_count = count;
    sqlite3_finalize(stmt);
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * chat_update —— 更新对话记录（改）
 * -----------------------------------------------------------------------------
 * 【SQL】UPDATE chat_history SET role=?, content=? WHERE id=?
 * 【说明】只更新 role 和 content，time 不变。
 *        WHERE id=? 限定只改指定行，不加 WHERE 会更新全表！
 * -----------------------------------------------------------------------------
 */
int chat_update(int id, int role, const char *content)
{
    sqlite3_stmt *stmt = NULL;
    int ret;

    if (!g_initialized || content == NULL) {
        return -1;
    }

    const char *sql = "UPDATE chat_history SET role=?, content=? WHERE id=?;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] chat_update prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    /* 绑定参数：?1=role ?2=content ?3=id */
    sqlite3_bind_int(stmt, 1, role);
    sqlite3_bind_text(stmt, 2, content, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, id);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] chat_update step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    /* sqlite3_changes 返回最近一次 UPDATE/DELETE 影响的行数。
     * 若为 0 表示没有 id 匹配的记录。 */
    int affected = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (affected == 0) {
        printf("[sqlite_db] chat_update: id=%d 不存在\n", id);
        return -1;
    }

    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * chat_delete —— 删除对话记录（删）
 * -----------------------------------------------------------------------------
 * 【SQL】DELETE FROM chat_history WHERE id=?
 * 【易错点】必须加 WHERE，否则 DELETE FROM chat_history 会清空全表！
 * -----------------------------------------------------------------------------
 */
int chat_delete(int id)
{
    sqlite3_stmt *stmt = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    const char *sql = "DELETE FROM chat_history WHERE id=?;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] chat_delete prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, id);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] chat_delete step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int affected = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (affected == 0) {
        printf("[sqlite_db] chat_delete: id=%d 不存在\n", id);
        return -1;
    }

    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * chat_clear —— 清空所有对话记录
 * -----------------------------------------------------------------------------
 * 【SQL】DELETE FROM chat_history
 * 【说明】无 WHERE 的 DELETE 清空全表，但自增 ID 不归零。
 *        想让 ID 从 1 重新开始需：DELETE FROM sqlite_sequence WHERE name='chat_history';
 * -----------------------------------------------------------------------------
 */
int chat_clear(void)
{
    char *errmsg = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    /* 清空表用 sqlite3_exec 即可，无需 prepare（没有用户参数） */
    ret = sqlite3_exec(g_db, "DELETE FROM chat_history;", NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] chat_clear 失败: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    /* 重置自增序列，让下次 INSERT 的 id 从 1 开始 */
    sqlite3_exec(g_db, "DELETE FROM sqlite_sequence WHERE name='chat_history';",
                 NULL, NULL, NULL);

    printf("[sqlite_db] chat_history 已清空\n");
    return 0;
}


/* =============================================================================
 * 六、kb_files 表 CRUD 实现（结构与 chat_history 类似，注释精简）
 * =============================================================================
 */


/* kb_add —— 插入知识库文件记录 */
int kb_add(const char *path, int type)
{
    sqlite3_stmt *stmt = NULL;
    int ret;
    char time_buf[32];
    int new_id;

    if (!g_initialized || path == NULL) {
        return -1;
    }

    get_current_time_str(time_buf, sizeof(time_buf));

    const char *sql = "INSERT INTO kb_files (path, type, create_time) VALUES (?, ?, ?);";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] kb_add prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, type);
    sqlite3_bind_text(stmt, 3, time_buf, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] kb_add step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    new_id = (int)sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(stmt);
    return new_id;
}


/* kb_query —— 查询知识库文件记录 */
int kb_query(kb_file_t *out, int max_count, int *actual_count)
{
    sqlite3_stmt *stmt = NULL;
    int ret;
    int count = 0;

    if (!g_initialized || out == NULL || actual_count == NULL) {
        return -1;
    }

    const char *sql = "SELECT id, path, type, create_time FROM kb_files ORDER BY id;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] kb_query prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        out[count].id = sqlite3_column_int(stmt, 0);

        const char *path_val = (const char *)sqlite3_column_text(stmt, 1);
        strncpy(out[count].path, path_val, sizeof(out[count].path) - 1);
        out[count].path[sizeof(out[count].path) - 1] = '\0';

        out[count].type = sqlite3_column_int(stmt, 2);

        const char *time_val = (const char *)sqlite3_column_text(stmt, 3);
        strncpy(out[count].create_time, time_val, sizeof(out[count].create_time) - 1);
        out[count].create_time[sizeof(out[count].create_time) - 1] = '\0';

        count++;
    }

    *actual_count = count;
    sqlite3_finalize(stmt);
    return 0;
}


/* kb_update —— 更新知识库文件记录 */
int kb_update(int id, const char *path, int type)
{
    sqlite3_stmt *stmt = NULL;
    int ret;

    if (!g_initialized || path == NULL) {
        return -1;
    }

    const char *sql = "UPDATE kb_files SET path=?, type=? WHERE id=?;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] kb_update prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, type);
    sqlite3_bind_int(stmt, 3, id);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] kb_update step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int affected = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (affected == 0) {
        printf("[sqlite_db] kb_update: id=%d 不存在\n", id);
        return -1;
    }
    return 0;
}


/* kb_delete —— 删除知识库文件记录 */
int kb_delete(int id)
{
    sqlite3_stmt *stmt = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    const char *sql = "DELETE FROM kb_files WHERE id=?;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] kb_delete prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, id);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] kb_delete step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int affected = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (affected == 0) {
        printf("[sqlite_db] kb_delete: id=%d 不存在\n", id);
        return -1;
    }
    return 0;
}


/* kb_clear —— 清空知识库文件记录 */
int kb_clear(void)
{
    char *errmsg = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    ret = sqlite3_exec(g_db, "DELETE FROM kb_files;", NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] kb_clear 失败: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    sqlite3_exec(g_db, "DELETE FROM sqlite_sequence WHERE name='kb_files';",
                 NULL, NULL, NULL);

    printf("[sqlite_db] kb_files 已清空\n");
    return 0;
}


/* =============================================================================
 * 七、perf_log 表 CRUD 实现
 * =============================================================================
 */


/* perf_log_add —— 插入性能日志 */
int perf_log_add(int model, double latency, size_t mem_peak)
{
    sqlite3_stmt *stmt = NULL;
    int ret;
    char time_buf[32];
    int new_id;

    if (!g_initialized) {
        return -1;
    }

    get_current_time_str(time_buf, sizeof(time_buf));

    const char *sql = "INSERT INTO perf_log (time, model, latency, mem_peak) VALUES (?, ?, ?, ?);";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] perf_log_add prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    /* 绑定参数。
     * sqlite3_bind_double：绑定浮点数 */
    sqlite3_bind_text(stmt, 1, time_buf, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, model);
    sqlite3_bind_double(stmt, 3, latency);
    /* size_t 在 64 位系统是 8 字节，SQLite 的 INTEGER 最大 8 字节，可安全存储。
     * 这里用 sqlite3_bind_int64 确保大值不溢出。 */
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)mem_peak);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] perf_log_add step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    new_id = (int)sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(stmt);
    return new_id;
}


/* perf_log_query —— 查询性能日志 */
int perf_log_query(perf_log_t *out, int max_count, int *actual_count)
{
    sqlite3_stmt *stmt = NULL;
    int ret;
    int count = 0;

    if (!g_initialized || out == NULL || actual_count == NULL) {
        return -1;
    }

    const char *sql = "SELECT id, time, model, latency, mem_peak FROM perf_log ORDER BY id;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] perf_log_query prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        out[count].id = sqlite3_column_int(stmt, 0);

        const char *time_val = (const char *)sqlite3_column_text(stmt, 1);
        strncpy(out[count].time, time_val, sizeof(out[count].time) - 1);
        out[count].time[sizeof(out[count].time) - 1] = '\0';

        out[count].model = sqlite3_column_int(stmt, 2);
        /* sqlite3_column_double：读取浮点数列 */
        out[count].latency = sqlite3_column_double(stmt, 3);
        /* sqlite3_column_int64：读取 64 位整数列 */
        out[count].mem_peak = (size_t)sqlite3_column_int64(stmt, 4);

        count++;
    }

    *actual_count = count;
    sqlite3_finalize(stmt);
    return 0;
}


/* perf_log_update —— 更新性能日志 */
int perf_log_update(int id, int model, double latency, size_t mem_peak)
{
    sqlite3_stmt *stmt = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    const char *sql = "UPDATE perf_log SET model=?, latency=?, mem_peak=? WHERE id=?;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] perf_log_update prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, model);
    sqlite3_bind_double(stmt, 2, latency);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)mem_peak);
    sqlite3_bind_int(stmt, 4, id);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] perf_log_update step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int affected = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (affected == 0) {
        printf("[sqlite_db] perf_log_update: id=%d 不存在\n", id);
        return -1;
    }
    return 0;
}


/* perf_log_delete —— 删除性能日志 */
int perf_log_delete(int id)
{
    sqlite3_stmt *stmt = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    const char *sql = "DELETE FROM perf_log WHERE id=?;";
    ret = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] perf_log_delete prepare 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, id);

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
        printf("[sqlite_db] perf_log_delete step 失败: %s\n", sqlite3_errmsg(g_db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int affected = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (affected == 0) {
        printf("[sqlite_db] perf_log_delete: id=%d 不存在\n", id);
        return -1;
    }
    return 0;
}


/* perf_log_clear —— 清空性能日志 */
int perf_log_clear(void)
{
    char *errmsg = NULL;
    int ret;

    if (!g_initialized) {
        return -1;
    }

    ret = sqlite3_exec(g_db, "DELETE FROM perf_log;", NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        printf("[sqlite_db] perf_log_clear 失败: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    sqlite3_exec(g_db, "DELETE FROM sqlite_sequence WHERE name='perf_log';",
                 NULL, NULL, NULL);

    printf("[sqlite_db] perf_log 已清空\n");
    return 0;
}
