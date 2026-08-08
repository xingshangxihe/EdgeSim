#ifndef SQLITE_DB_H
#define SQLITE_DB_H

/*
 * =============================================================================
 * EdgeSim SQLite 数据库封装模块公共头文件  sqlite_db.h
 * =============================================================================
 * 【文件作用】
 *   声明 sqlite_db 模块对外提供的数据类型与函数接口。
 *   封装 SQLite3，为 EdgeSim 提供本地持久化存储：
 *     - chat_history：对话历史（用户提问 + AI 回答）
 *     - kb_files    ：RAG 知库文件索引（TXT/MD/PDF/图片路径）
 *     - perf_log    ：性能测试日志（推理延迟、内存峰值）
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.3 节「sqlite_db（数据库）」
 *
 * 【模块职责】
 *   - db_init()  ：打开数据库，创建三张表
 *   - db_close() ：关闭数据库
 *   - 三表各提供 5 个 CRUD 接口：add(增) / query(查) / update(改) / delete(删) / clear(清空)
 *
 * 【设计约束】
 *   仅封装 SQLite3，不处理进程调度与内存管理（归 business 层其他模块）
 * =============================================================================
 */

#include <stddef.h>   /* size_t 定义 */

#ifdef __cplusplus
extern "C" {
#endif


/* =============================================================================
 * 一、记录结构体定义（对应三张表的行结构）
 * ============================================================================= */

/*
 * 对话历史记录（对应 chat_history 表）
 * 【字段说明】
 *   id      ：自增主键
 *   time    ：时间字符串 "YYYY-MM-DD HH:MM:SS"
 *   role    ：角色 0=用户(user) 1=AI助手(assistant)
 *   content ：消息内容（最长 4095 字符）
 */
#define CHAT_CONTENT_MAX  4096   /* content 字段最大长度 */

typedef struct {
    int  id;                                   /* 自增主键 */
    char time[32];                             /* 时间字符串 */
    int  role;                                 /* 0=user 1=assistant */
    char content[CHAT_CONTENT_MAX];            /* 消息内容 */
} chat_record_t;


/*
 * 知识库文件记录（对应 kb_files 表）
 * 【字段说明】
 *   id          ：自增主键
 *   path        ：文件绝对路径
 *   type        ：文件类型 0=txt 1=md 2=pdf 3=image
 *   create_time ：导入时间
 */
#define KB_PATH_MAX  256   /* path 字段最大长度 */

typedef struct {
    int  id;                                   /* 自增主键 */
    char path[KB_PATH_MAX];                    /* 文件路径 */
    int  type;                                 /* 0=txt 1=md 2=pdf 3=image */
    char create_time[32];                      /* 导入时间 */
} kb_file_t;


/*
 * 性能日志记录（对应 perf_log 表）
 * 【字段说明】
 *   id        ：自增主键
 *   time      ：记录时间
 *   model     ：模型 ID 0=LLM 1=OCR 2=ASR
 *   latency   ：推理延迟（毫秒）
 *   mem_peak  ：内存峰值（MB）
 */
typedef struct {
    int    id;                                 /* 自增主键 */
    char   time[32];                           /* 记录时间 */
    int    model;                              /* 0=LLM 1=OCR 2=ASR */
    double latency;                            /* 延迟(ms) */
    size_t mem_peak;                           /* 内存峰值(MB) */
} perf_log_t;


/* =============================================================================
 * 二、数据库生命周期接口
 * =============================================================================
 */

/*
 * db_init —— 打开数据库并创建表
 * 【参数】db_path：数据库文件路径（如 "edgesim.db"）
 *         传 ":memory:" 可创建内存数据库（测试用，进程退出即销毁）
 * 【返回值】0=成功，-1=失败
 */
int db_init(const char *db_path);

/*
 * db_close —— 关闭数据库
 * 【返回值】0=成功，-1=失败
 */
int db_close(void);


/* =============================================================================
 * 三、chat_history 表 CRUD 接口（对话历史）
 * =============================================================================
 */

/*
 * chat_save —— 插入一条对话记录（增）
 * 【参数】time_str：时间字符串（NULL 则自动取当前时间）
 * 【参数】role：0=用户 1=AI
 * 【参数】content：消息内容
 * 【返回值】>0=新记录 ID，-1=失败
 */
int chat_save(const char *time_str, int role, const char *content);

/*
 * chat_query —— 查询对话记录（查）
 * 【参数】out：输出数组（调用者分配）
 * 【参数】max_count：数组最大容量
 * 【参数】actual_count：输出，实际查询到的条数
 * 【返回值】0=成功，-1=失败
 */
int chat_query(chat_record_t *out, int max_count, int *actual_count);

/*
 * chat_update —— 更新对话记录（改）
 * 【参数】id：要更新的记录 ID
 * 【参数】role：新角色
 * 【参数】content：新内容
 * 【返回值】0=成功，-1=失败
 */
int chat_update(int id, int role, const char *content);

/*
 * chat_delete —— 删除指定对话记录（删）
 * 【参数】id：要删除的记录 ID
 * 【返回值】0=成功，-1=失败
 */
int chat_delete(int id);

/*
 * chat_clear —— 清空所有对话记录（清空）
 * 【返回值】0=成功，-1=失败
 */
int chat_clear(void);


/* =============================================================================
 * 四、kb_files 表 CRUD 接口（知识库文件）
 * =============================================================================
 */

/*
 * kb_add —— 插入一条知识库文件记录（增）
 * 【参数】path：文件路径
 * 【参数】type：文件类型 0=txt 1=md 2=pdf 3=image
 * 【返回值】>0=新记录 ID，-1=失败
 */
int kb_add(const char *path, int type);

/*
 * kb_query —— 查询知识库文件记录（查）
 * 【返回值】0=成功，-1=失败
 */
int kb_query(kb_file_t *out, int max_count, int *actual_count);

/*
 * kb_update —— 更新知识库文件记录（改）
 * 【返回值】0=成功，-1=失败
 */
int kb_update(int id, const char *path, int type);

/*
 * kb_delete —— 删除指定知识库文件记录（删）
 * 【返回值】0=成功，-1=失败
 */
int kb_delete(int id);

/*
 * kb_clear —— 清空所有知识库文件记录（清空）
 * 【返回值】0=成功，-1=失败
 */
int kb_clear(void);


/* =============================================================================
 * 五、perf_log 表 CRUD 接口（性能日志）
 * =============================================================================
 */

/*
 * perf_log_add —— 插入一条性能日志（增）
 * 【参数】model：模型 ID 0=LLM 1=OCR 2=ASR
 * 【参数】latency：推理延迟（毫秒）
 * 【参数】mem_peak：内存峰值（MB）
 * 【返回值】>0=新记录 ID，-1=失败
 */
int perf_log_add(int model, double latency, size_t mem_peak);

/*
 * perf_log_query —— 查询性能日志（查）
 * 【返回值】0=成功，-1=失败
 */
int perf_log_query(perf_log_t *out, int max_count, int *actual_count);

/*
 * perf_log_update —— 更新性能日志（改）
 * 【返回值】0=成功，-1=失败
 */
int perf_log_update(int id, int model, double latency, size_t mem_peak);

/*
 * perf_log_delete —— 删除指定性能日志（删）
 * 【返回值】0=成功，-1=失败
 */
int perf_log_delete(int id);

/*
 * perf_log_clear —— 清空所有性能日志（清空）
 * 【返回值】0=成功，-1=失败
 */
int perf_log_clear(void);


#ifdef __cplusplus
}
#endif

#endif /* SQLITE_DB_H */
