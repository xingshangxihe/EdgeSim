/*
 * =============================================================================
 * EdgeSim 内存仿真模块实现文件  hardware_sim.c
 * =============================================================================
 * 【文件作用】
 *   实现 hardware_sim.h 中声明的全部接口。本文件是 EdgeSim 的核心底层代码，
 *   模拟低成本 Linux 开发板（全志 T153 / 瑞芯微 RV1106）的小内存环境，
 *   在 PC 上即可测试 AI 模型加载/卸载策略，避免真机 OOM 崩溃。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.1 节
 *
 * 【四大核心接口】
 *   1. mem_sim_get_usage()    —— 内存读取：实时返回当前使用率
 *   2. mem_sim_alloc()        —— 内存上限限制：超 90% 自动回收，超 100% 拒绝
 *   3. mem_sim_unload_idle()  —— 闲置模型释放：按 access_count 卸载最闲模型
 *   4. mem_sim_gen_report()   —— 性能报表：输出模型体积/延迟/峰值到文件
 *
 * 【线程安全】
 *   所有接口通过 pthread_mutex_t 互斥锁保护全局变量，可被多线程安全调用。
 * =============================================================================
 */


/* =============================================================================
 * 一、头文件包含区（每个头文件作用逐条说明）
 * ============================================================================= */
#include "hardware_sim.h"   /* 本模块公共接口声明 */

#include <stdio.h>          /* 标准输入输出：fopen/fprintf/fgets/printf 等 */
#include <stdlib.h>         /* 标准库：exit 等 */
#include <string.h>         /* 字符串操作：memset/strncpy/strstr/strtol */

#include <time.h>           /* 时间函数：clock_gettime/localtime/strftime */
#include <pthread.h>        /* POSIX 线程：pthread_mutex_lock/unlock，链接需 -lpthread */

#include <sys/resource.h>   /* getrusage()：获取进程资源使用量（如最大驻留内存） */
#include <unistd.h>         /* POSIX 标准：access() 等系统调用 */


/* =============================================================================
 * 二、常量与宏定义
 * ============================================================================= */

/* 内存回收阈值比例：当已用内存 >= 上限的 90% 时，触发自动回收闲置模型。
 * 0.9 是经验值：太低（如 0.5）会频繁卸载模型影响性能；
 *              太高（如 0.99）来不及回收就 OOM。 */
#define MEM_SIM_THRESHOLD_RATIO  0.9

/* 模型表容量，等于 model_id_t 枚举中的 MODEL_ID_MAX。
 * 用宏而非常量是为了数组声明和循环边界都能用。 */
#define MEM_SIM_MAX_MODELS       MODEL_ID_MAX


/* =============================================================================
 * 三、模块内部全局变量（均声明为 static，仅本文件可见）
 * =============================================================================
 * 【设计文档要求】
 *   g_total_mem_limit   ：全局内存上限（默认 512MB）
 *   g_current_used_mem  ：当前已用内存
 * 【扩展变量】
 *   g_model_table       ：三模型内存信息表
 *   g_mem_mutex         ：互斥锁，保证多线程并发安全
 *   g_global_peak_mem   ：全局内存峰值（用于报表）
 *   g_initialized       ：初始化标志，防止未初始化就调用其它接口
 * ========================================================================== */

/* 全局内存上限（MB），由 mem_sim_init() 设置，默认 512MB，支持小数 */
static double g_total_mem_limit = 512.0;

/* 当前已用内存（MB），每次 alloc/free 实时更新，支持小数 */
static double g_current_used_mem = 0.0;

/* 三模型内存信息表，索引即 model_id。
 * 静态全局变量默认零初始化（C 标准保证），即所有 in_use 初始为 0。 */
static model_mem_info_t g_model_table[MEM_SIM_MAX_MODELS];

/* 互斥锁，PTHREAD_MUTEX_INITIALIZER 是 POSIX 静态初始化宏，
 * 无需调用 pthread_mutex_init()，程序加载即就绪。 */
static pthread_mutex_t g_mem_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 全局内存历史峰值（MB），仅增不减，用于报表统计 */
static double g_global_peak_mem = 0.0;

/* 初始化标志：0=未初始化，1=已初始化 */
static int g_initialized = 0;


/* =============================================================================
 * 四、内部辅助函数（static 修饰，仅本文件可用）
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：get_current_time_sec
 * -----------------------------------------------------------------------------
 * 【作用】获取当前 Unix 时间戳（单位：秒）
 * 【返回值】long long 类型的秒数（1970-01-01 至今）
 * 【Linux API 说明】
 *   clock_gettime(CLOCK_REALTIME, &ts)
 *     - POSIX 标准函数，比 time() 精度更高（纳秒级）
 *     - CLOCK_REALTIME：系统实时时钟，可被 NTP 调整，适合记录时间戳
 *     - 替代方案 CLOCK_MONOTONIC：单调时钟，不受 NTP 影响，适合测耗时
 *   struct timespec { time_t tv_sec; long tv_nsec; }
 *     - tv_sec：秒部分
 *     - tv_nsec：纳秒部分（0~999999999）
 * 【易错点】
 *   1. clock_gettime 在某些老旧 Linux 需链接 -lrt，glibc 2.17+ 已内建
 *   2. 返回值不检查是因为 CLOCK_REALTIME 不会失败（除非 ts 指针非法）
 * -----------------------------------------------------------------------------
 */
static long long get_current_time_sec(void)
{
    struct timespec ts;                                  /* 时间结构体 */

    /* 获取当前实时时间，结果填入 ts */
    clock_gettime(CLOCK_REALTIME, &ts);

    /* 只取秒部分，纳秒部分丢弃（本模块闲置判定精度到秒足够） */
    return (long long)ts.tv_sec;
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：find_idlest_model
 * -----------------------------------------------------------------------------
 * 【作用】在已加载模型中找出"最闲置"的一个
 * 【返回值】找到返回 model_id（0~2），无已加载模型返回 -1
 * 【闲置判定规则】
 *   1. 主标准：access_count 最小（访问次数最少 = 最闲置）
 *   2. 次标准：access_count 相同时，last_access_sec 最小（最久未访问）
 * 【前提】调用前已持有 g_mem_mutex 互斥锁
 * 【易错点】
 *   必须跳过 in_use==0 的槽位，否则会误选未加载模型
 * -----------------------------------------------------------------------------
 */
static int find_idlest_model(void)
{
    int idlest_id = -1;                                  /* 初始化为 -1 表示未找到 */
    long long min_access = 0;                            /* 记录最小访问次数 */
    long long min_time = 0;                              /* 记录最旧访问时间 */
    int i;                                               /* 循环变量 */

    /* 遍历所有模型槽位 */
    for (i = 0; i < MEM_SIM_MAX_MODELS; i++) {
        /* 跳过未加载的槽位 */
        if (g_model_table[i].in_use == 0) {
            continue;
        }

        /* 第一个找到的已加载模型直接作为初始候选 */
        if (idlest_id == -1) {
            idlest_id  = i;
            min_access = g_model_table[i].access_count;
            min_time   = g_model_table[i].last_access_sec;
            continue;
        }

        /* 比较主标准：access_count 更小则更闲置 */
        if (g_model_table[i].access_count < min_access) {
            idlest_id  = i;
            min_access = g_model_table[i].access_count;
            min_time   = g_model_table[i].last_access_sec;
        }
        /* 主标准并列时，比较次标准：last_access_sec 更小（更久未访问）则更闲置 */
        else if (g_model_table[i].access_count == min_access &&
                 g_model_table[i].last_access_sec < min_time) {
            idlest_id  = i;
            min_time   = g_model_table[i].last_access_sec;
        }
    }

    return idlest_id;                                    /* 返回最闲置模型 ID 或 -1 */
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：read_proc_rss_mb
 * -----------------------------------------------------------------------------
 * 【作用】读取当前进程实际驻留物理内存（RSS），单位 MB
 * 【返回值】RSS 内存（MB），读取失败返回 0
 * 【Linux API 说明】
 *   /proc/self/status 是 Linux 内核提供的虚拟文件，包含当前进程状态。
 *   其中 VmRSS 行格式：  VmRSS:     12345 kB
 *   解析步骤：
 *     1. fopen("/proc/self/status", "r") 打开文件
 *     2. 逐行 fgets 读取
 *     3. strstr 查找 "VmRSS:" 前缀
 *     4. strtol 提取数字部分
 * 【易错点】
 *   1. /proc 是 Linux 专属，Windows 无此文件，跨平台需条件编译
 *   2. fopen 后必须 fclose，否则文件描述符泄漏
 *   3. VmRSS 单位是 kB（注意是小写 k），需换算成 MB
 * -----------------------------------------------------------------------------
 */
static size_t read_proc_rss_mb(void)
{
    FILE *fp;                                            /* 文件指针 */
    char line[256];                                      /* 行缓冲区 */
    long rss_kb = 0;                                     /* RSS（KB） */

    /* 以只读模式打开 /proc/self/status。
     * /proc/self 是指向当前进程 /proc/<pid> 的符号链接，无需知道自己的 PID。 */
    fp = fopen("/proc/self/status", "r");
    if (fp == NULL) {
        /* 打开失败（非 Linux 环境或权限不足），返回 0 表示无法读取 */
        return 0;
    }

    /* 逐行读取文件，每行最多 255 字符 */
    while (fgets(line, sizeof(line), fp) != NULL) {
        /* strstr 查找子串 "VmRSS:"，找到说明本行是 RSS 信息 */
        if (strstr(line, "VmRSS:") != NULL) {
            /* 行格式示例："VmRSS:     12345 kB"
             * strtol 从字符串中解析第一个整数，自动跳过前导空格。
             * 第二个参数 NULL 表示不关心解析结束位置。 */
            rss_kb = strtol(line + 6, NULL, 10);
            /* line+6 跳过 "VmRSS:" 6 个字符，指向数字部分 */
            break;                                       /* 找到后无需继续读 */
        }
    }

    fclose(fp);                                          /* 关闭文件，防止描述符泄漏 */

    /* KB 转 MB：1024 KB = 1 MB。
     * 使用 (rss_kb + 1023) / 1024 实现向上取整，避免小内存被舍入为 0。 */
    return (size_t)((rss_kb + 1023) / 1024);
}


/* =============================================================================
 * 五、对外接口实现（逐函数详解）
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 接口 1：mem_sim_init —— 初始化内存仿真器
 * -----------------------------------------------------------------------------
 * 【参数】limit_mb：全局内存上限（MB），推荐 256 / 512 / 1024
 * 【返回值】0=成功，-1=失败（参数为 0 或过大）
 * 【作用】
 *   1. 设置全局内存上限 g_total_mem_limit
 *   2. 清零模型表 g_model_table（重置所有槽位为空闲）
 *   3. 重置已用内存、峰值、初始化标志
 * 【易错点】
 *   重复调用会重置所有状态，已加载的模型信息会丢失。正式使用前只调一次。
 * -----------------------------------------------------------------------------
 */
int mem_sim_init(double limit_mb)
{
    /* 参数校验：上限为 0 或超过 4096MB（4GB）视为非法。
     * 4096 是经验上限，超出则可能不是仿真低端设备了。 */
    if (limit_mb == 0 || limit_mb > 4096) {
        return -1;
    }

    /* 加锁保护全局变量。
     * pthread_mutex_lock 会阻塞直到拿到锁；返回 0 表示成功。 */
    pthread_mutex_lock(&g_mem_mutex);

    /* 设置内存上限 */
    g_total_mem_limit = limit_mb;

    /* 重置已用内存 */
    g_current_used_mem = 0;

    /* 重置全局峰值 */
    g_global_peak_mem = 0;

    /* 清零模型表。
     * memset 把 g_model_table 的每个字节都置为 0，
     * 等价于所有 in_use=0, alloc_mb=0, access_count=0 ... */
    memset(g_model_table, 0, sizeof(g_model_table));

    /* 标记为已初始化 */
    g_initialized = 1;

    /* 解锁。
     * pthread_mutex_unlock 释放锁，让其他线程可以获取。
     * 易错点：加锁后必须有对应解锁，否则死锁。建议成对编写。 */
    pthread_mutex_unlock(&g_mem_mutex);

    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 2：mem_sim_alloc —— 申请内存（内存上限限制核心接口）
 * -----------------------------------------------------------------------------
 * 【参数】size_mb：申请内存大小（MB）
 * 【参数】model_id：模型 ID（MODEL_ID_LLM / OCR / ASR）
 * 【返回值】0=成功，-1=失败
 * 【作用】
 *   1. 校验参数合法性
 *   2. 若申请后使用率将达 90% 阈值，先自动调用 mem_sim_unload_idle() 回收
 *   3. 若申请后超过 100% 硬上限，拒绝并返回 -1
 *   4. 更新模型表与已用内存
 * 【易错点】
 *   1. 同一 model_id 重复 alloc 会先释放旧内存再重新分配
 *   2. size_mb 为 0 视为非法
 * -----------------------------------------------------------------------------
 */
int mem_sim_alloc(double size_mb, int model_id)
{
    /* 返回值缓存变量 */
    int ret = 0;
    /* 预测申请后的总内存，用于阈值判断 */
    double predicted_used;

    /* ---- 参数校验 ---- */
    /* model_id 必须在有效范围内 */
    if (model_id < 0 || model_id >= MEM_SIM_MAX_MODELS) {
        return -1;
    }
    /* 申请大小必须大于 0 */
    if (size_mb == 0) {
        return -1;
    }
    /* 未初始化则拒绝操作 */
    if (g_initialized == 0) {
        return -1;
    }

    /* ---- 加锁进入临界区 ---- */
    pthread_mutex_lock(&g_mem_mutex);

    /* 若该模型已加载，先释放旧内存（避免重复计数） */
    if (g_model_table[model_id].in_use == 1) {
        g_current_used_mem -= g_model_table[model_id].alloc_mb;
        g_model_table[model_id].in_use = 0;
    }

    /* ---- 阈值判定与自动回收 ---- */
    predicted_used = g_current_used_mem + size_mb;

    /* 若预测使用率 >= 90% 阈值，尝试自动回收闲置模型。
     * 注意：这里用 >= 而非 >，确保恰好达到 90% 时也触发回收。 */
    if (predicted_used >= g_total_mem_limit * MEM_SIM_THRESHOLD_RATIO) {
        /* 循环回收，直到使用率降到 90% 以下或无模型可回收。
         * 用 while 而非 if 是因为可能需要回收多个模型。 */
        while (predicted_used >= g_total_mem_limit * MEM_SIM_THRESHOLD_RATIO) {
            int idle_id;                                /* 待回收的模型 ID */
            double freed_mb;                            /* 本次回收的内存 */

            /* 调用内部逻辑查找并回收最闲置模型。
             * 注意：这里不能直接调 mem_sim_unload_idle()，因为它会重复加锁导致死锁。
             * 我们已在锁内，必须用不加锁的内部逻辑。 */
            idle_id = find_idlest_model();
            if (idle_id < 0) {
                /* 无可回收模型，退出循环 */
                break;
            }

            /* 记录回收的内存大小 */
            freed_mb = g_model_table[idle_id].alloc_mb;
            /* 清除占用标志 */
            g_model_table[idle_id].in_use = 0;
            /* 减少已用内存 */
            g_current_used_mem -= freed_mb;
            /* 重新计算预测值 */
            predicted_used = g_current_used_mem + size_mb;

            /* 打印回收日志，便于调试 */
            printf("[mem_sim] 自动卸载闲置模型 ID=%d，回收 %.1f MB\n",
                   idle_id, freed_mb);
        }
    }

    /* ---- 硬上限判定 ---- */
    /* 即使回收后仍超过 100% 上限，则拒绝申请。
     * 这是最后一道防线，防止 OOM。 */
    if (predicted_used > g_total_mem_limit) {
        printf("[mem_sim] 申请 %.1f MB 失败：将超过硬上限 %.1f MB（当前已用 %.1f MB）\n",
               size_mb, g_total_mem_limit, g_current_used_mem);
        ret = -1;
        /* 跳转到解锁标签，确保锁被释放 */
        goto unlock_and_return;
    }

    /* ---- 执行分配 ---- */
    /* 填充模型表项 */
    g_model_table[model_id].in_use          = 1;
    g_model_table[model_id].model_id        = model_id;
    g_model_table[model_id].alloc_mb        = size_mb;
    g_model_table[model_id].last_access_sec = get_current_time_sec();
    g_model_table[model_id].access_count    = 1;        /* 首次加载算 1 次访问 */

    /* 若未设置过模型名，给个默认名 */
    if (g_model_table[model_id].model_name[0] == '\0') {
        const char *default_name = "unknown";
        strncpy(g_model_table[model_id].model_name, default_name,
                sizeof(g_model_table[model_id].model_name) - 1);
        /* strncpy 不保证 '\0' 结尾，手动补一下，新手易错点 */
        g_model_table[model_id].model_name[sizeof(g_model_table[model_id].model_name) - 1] = '\0';
    }

    /* 更新已用内存 */
    g_current_used_mem = predicted_used;

    /* 更新该模型峰值 */
    if (size_mb > g_model_table[model_id].peak_mem_mb) {
        g_model_table[model_id].peak_mem_mb = size_mb;
    }

    /* 更新全局峰值 */
    if (g_current_used_mem > g_global_peak_mem) {
        g_global_peak_mem = g_current_used_mem;
    }

    printf("[mem_sim] 模型 ID=%d 申请 %.1f MB 成功，当前已用 %.1f / %.1f MB\n",
           model_id, size_mb, g_current_used_mem, g_total_mem_limit);

unlock_and_return:
    /* 统一解锁出口。
     * 用 goto 跳转到此处是 Linux 内核代码常见写法，保证所有分支都能解锁。 */
    pthread_mutex_unlock(&g_mem_mutex);
    return ret;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 3：mem_sim_free —— 释放指定模型内存
 * -----------------------------------------------------------------------------
 * 【参数】model_id：要释放的模型 ID
 * 【返回值】0=成功，-1=失败（模型未加载或参数非法）
 * 【作用】
 *   1. 清除模型占用标志
 *   2. 从已用内存中扣减该模型的 alloc_mb
 *   3. 注意：模型名等元信息保留，便于后续报表统计
 * -----------------------------------------------------------------------------
 */
int mem_sim_free(int model_id)
{
    /* 参数校验 */
    if (model_id < 0 || model_id >= MEM_SIM_MAX_MODELS) {
        return -1;
    }
    if (g_initialized == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_mem_mutex);

    /* 检查该模型是否已加载 */
    if (g_model_table[model_id].in_use == 0) {
        pthread_mutex_unlock(&g_mem_mutex);
        return -1;                                      /* 未加载，无法释放 */
    }

    /* 扣减已用内存 */
    g_current_used_mem -= g_model_table[model_id].alloc_mb;

    /* 清除占用标志，但保留 model_name / peak_mem_mb 等统计信息 */
    g_model_table[model_id].in_use   = 0;
    g_model_table[model_id].alloc_mb = 0;

    printf("[mem_sim] 模型 ID=%d 已释放，当前已用 %.1f / %.1f MB\n",
           model_id, g_current_used_mem, g_total_mem_limit);

    pthread_mutex_unlock(&g_mem_mutex);
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 4：mem_sim_get_usage —— 获取内存使用快照（内存读取接口）
 * -----------------------------------------------------------------------------
 * 【参数】usage：输出参数，调用者传入 mem_usage_t 变量地址
 * 【返回值】0=成功，-1=失败（参数为空或未初始化）
 * 【作用】
 *   实时返回当前内存上限、已用、剩余、使用率百分比。
 *   这是「内存读取」核心接口，UI 层通过它驱动内存进度条显示。
 * -----------------------------------------------------------------------------
 */
int mem_sim_get_usage(mem_usage_t *usage)
{
    /* 参数校验：指针不能为空 */
    if (usage == NULL) {
        return -1;
    }
    if (g_initialized == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_mem_mutex);

    /* 填充各字段 */
    usage->total_limit_mb = g_total_mem_limit;
    usage->used_mb        = g_current_used_mem;
    /* 剩余 = 上限 - 已用。
     * 注意：g_current_used_mem 不会超过 g_total_mem_limit（alloc 时已校验），
     * 且两者都是 double，减法不会出现无符号下溢问题。 */
    usage->free_mb        = g_total_mem_limit - g_current_used_mem;
    /* 使用率百分比 = 已用 / 上限 * 100。
     * 两者已是 double，直接除法即可保留小数精度。 */
    usage->usage_percent  = g_current_used_mem / g_total_mem_limit * 100.0;

    pthread_mutex_unlock(&g_mem_mutex);
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 4.5：mem_sim_get_model_mb —— 获取指定模型当前占用内存（阶段 2 新增）
 * -----------------------------------------------------------------------------
 * 【参数】model_id：模型 ID（MODEL_ID_LLM / OCR / ASR）
 * 【返回值】>=0.0=当前占用 MB（未加载返回 0），-1.0=失败（参数非法或未初始化）
 * 【作用】
 *   UI 层内存监控面板需要分别显示 LLM/OCR/ASR 各自占用，
 *   而 mem_sim_get_usage 只返回总量，无法区分模型。
 *   本接口在锁内读取模型表对应槽位的 alloc_mb 返回。
 * 【设计说明】
 *   1. 不暴露内部模型表，只返回一个数值，保持封装性
 *   2. 未加载模型的 alloc_mb 为 0，直接返回 0 即表示"未占用"
 *   3. 返回 -1.0 表示参数非法/未初始化，调用方应视为读取失败
 *   4. 返回 double：alloc_mb 支持小数（如 200.5MB），UI 可显示真实体积
 * -----------------------------------------------------------------------------
 */
double mem_sim_get_model_mb(int model_id)
{
    double mb;                                      /* 该模型当前占用（MB） */

    /* 参数校验：model_id 必须在有效范围内 */
    if (model_id < 0 || model_id >= MEM_SIM_MAX_MODELS) {
        return -1.0;
    }
    /* 未初始化则拒绝读取 */
    if (g_initialized == 0) {
        return -1.0;
    }

    /* 加锁进入临界区，读取模型表对应槽位 */
    pthread_mutex_lock(&g_mem_mutex);
    /* 未加载模型的 alloc_mb 恒为 0（alloc/free 中维护），直接读即可 */
    mb = g_model_table[model_id].alloc_mb;
    pthread_mutex_unlock(&g_mem_mutex);

    return mb;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 4.6：mem_sim_set_model_name —— 设置模型显示名称（阶段 3 新增）
 * -----------------------------------------------------------------------------
 * 【参数】model_id：模型 ID（MODEL_ID_LLM / OCR / ASR）
 * 【参数】name    ：模型名称字符串（如 "Qwen2.5-1.5B"）
 * 【返回值】0=成功，-1=失败（参数非法）
 * 【作用】
 *   mem_sim_gen_report 生成报表时模型名显示 "unknown"，信息不完整。
 *   本接口允许调用方（ui_pipeline 收到 MODEL_LOADED 时）设置真实名称。
 * 【设计说明】
 *   1. 名称写入模型表 model_name[32] 字段
 *   2. strncpy 限长 + 手动补 '\0'，防止越界（31 字符 + 1 个结束符）
 *   3. 名称在模型卸载后保留（mem_sim_free 不清空），便于报表统计
 * -----------------------------------------------------------------------------
 */
int mem_sim_set_model_name(int model_id, const char *name)
{
    /* 参数校验：model_id 与 name 都不能非法 */
    if (model_id < 0 || model_id >= MEM_SIM_MAX_MODELS || name == NULL) {
        return -1;
    }

    /* 加锁保护模型表 */
    pthread_mutex_lock(&g_mem_mutex);

    /* 复制名称到 model_name 字段（限长 31 字符 + 手动补 '\0'） */
    strncpy(g_model_table[model_id].model_name, name,
            sizeof(g_model_table[model_id].model_name) - 1);
    g_model_table[model_id].model_name[sizeof(g_model_table[model_id].model_name) - 1] = '\0';

    pthread_mutex_unlock(&g_mem_mutex);
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 5：mem_sim_unload_idle —— 卸载最闲置模型（闲置模型释放接口）
 * -----------------------------------------------------------------------------
 * 【返回值】>=0=被卸载的 model_id，-1=无可卸载模型
 * 【作用】
 *   查找当前已加载模型中最闲置的一个（access_count 最小），释放其内存。
 *   这是「闲置模型释放」核心接口，由 mem_sim_alloc 在阈值触发时自动调用，
 *   也可由外部主动调用以腾出空间。
 * -----------------------------------------------------------------------------
 */
int mem_sim_unload_idle(void)
{
    int idle_id;                                        /* 待卸载模型 ID */
    double freed_mb;                                    /* 回收的内存 */

    if (g_initialized == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_mem_mutex);

    /* 查找最闲置模型 */
    idle_id = find_idlest_model();
    if (idle_id < 0) {
        /* 无已加载模型可卸载 */
        pthread_mutex_unlock(&g_mem_mutex);
        printf("[mem_sim] 无可卸载的闲置模型\n");
        return -1;
    }

    /* 记录回收内存 */
    freed_mb = g_model_table[idle_id].alloc_mb;
    /* 清除占用 */
    g_model_table[idle_id].in_use   = 0;
    g_model_table[idle_id].alloc_mb = 0;
    /* 扣减已用内存 */
    g_current_used_mem -= freed_mb;

    printf("[mem_sim] 主动卸载闲置模型 ID=%d，回收 %.1f MB，当前已用 %.1f / %.1f MB\n",
           idle_id, freed_mb, g_current_used_mem, g_total_mem_limit);

    pthread_mutex_unlock(&g_mem_mutex);
    return idle_id;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 6：mem_sim_gen_report —— 生成性能报表（性能报表接口）
 * -----------------------------------------------------------------------------
 * 【参数】path：报表文件输出路径
 * 【返回值】0=成功，-1=失败
 * 【作用】
 *   把以下信息写入文本文件：
 *   - 内存上限、当前使用、峰值
 *   - 每个模型的：名称、当前占用、峰值、最近延迟、访问次数
 *   - 当前进程实际 RSS（来自 /proc/self/status）
 *   - getrusage 获取的资源信息
 * 【Linux API 说明】
 *   fopen(path, "w")：以写入模式打开文件，文件不存在则创建，存在则清空
 *   fprintf(fp, ...)：向文件写入格式化字符串，用法同 printf
 *   getrusage(RUSAGE_SELF, &ru)：获取调用进程的资源统计
 *     RUSAGE_SELF 表示当前进程本身（不含子进程）
 *     ru.ru_maxrss：进程峰值驻留内存（单位 KB，Linux 下）
 * -----------------------------------------------------------------------------
 */
int mem_sim_gen_report(const char *path)
{
    FILE *fp;                                           /* 文件指针 */
    int i;                                              /* 循环变量 */
    char time_buf[64];                                  /* 时间字符串缓冲区 */
    time_t now;                                         /* 当前时间 */
    struct tm *tm_info;                                 /* 分解时间结构体 */
    struct rusage ru;                                   /* 资源使用结构体 */
    size_t rss_mb;                                      /* 当前 RSS（MB） */

    /* 参数校验 */
    if (path == NULL) {
        return -1;
    }
    if (g_initialized == 0) {
        return -1;
    }

    /* 打开报表文件。"w" 表示写入模式：文件存在则清空，不存在则创建。 */
    fp = fopen(path, "w");
    if (fp == NULL) {
        /* 文件创建失败：路径不存在、权限不足、磁盘满等 */
        printf("[mem_sim] 报表文件创建失败：%s\n", path);
        return -1;
    }

    /* ---- 获取当前时间字符串 ---- */
    now = time(NULL);                                   /* 获取当前 Unix 时间戳 */
    /* localtime 把 time_t 转成分解时间 struct tm（年月日时分秒）。
     * 注意：localtime 返回静态变量指针，非线程安全；多线程环境用 localtime_r。 */
    tm_info = localtime(&now);
    /* strftime 按指定格式把 struct tm 格式化为字符串。
     * %Y-%m-%d %H:%M:%S 例：2026-07-25 14:30:00 */
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* ---- 获取进程实际内存 ---- */
    rss_mb = read_proc_rss_mb();

    /* ---- 获取进程资源统计 ---- */
    /* getrusage(RUSAGE_SELF, &ru) 填充 ru 结构体。
     * RUSAGE_SELF：统计当前进程自身（不含子进程）。
     * 返回 0 表示成功。 */
    getrusage(RUSAGE_SELF, &ru);

    /* 加锁读取全局状态（避免打印过程中状态变化） */
    pthread_mutex_lock(&g_mem_mutex);

    /* ---- 写入报表头部 ---- */
    fprintf(fp, "=====================================================\n");
    fprintf(fp, "  EdgeSim 性能报表  生成时间: %s\n", time_buf);
    fprintf(fp, "=====================================================\n\n");

    /* ---- 写入内存总览 ---- */
    fprintf(fp, "【一、内存仿真总览】\n");
    fprintf(fp, "  全局内存上限:   %.1f MB\n", g_total_mem_limit);
    fprintf(fp, "  当前已用内存:   %.1f MB\n", g_current_used_mem);
    fprintf(fp, "  当前剩余内存:   %.1f MB\n", g_total_mem_limit - g_current_used_mem);
    fprintf(fp, "  全局内存峰值:   %.1f MB\n", g_global_peak_mem);
    fprintf(fp, "  当前使用率:     %.2f%%\n",
            g_current_used_mem / g_total_mem_limit * 100.0);
    fprintf(fp, "  回收阈值比例:   %.0f%%\n", MEM_SIM_THRESHOLD_RATIO * 100);
    fprintf(fp, "\n");

    /* ---- 写入模型明细 ---- */
    fprintf(fp, "【二、各模型内存明细】\n");
    fprintf(fp, "  %-4s %-20s %-8s %-10s %-8s %-10s\n",
            "ID", "模型名称", "状态", "峰值(MB)", "延迟(ms)", "访问次数");
    fprintf(fp, "  %-4s %-20s %-8s %-10s %-8s %-10s\n",
            "----", "--------------------", "--------", "----------",
            "--------", "----------");

    for (i = 0; i < MEM_SIM_MAX_MODELS; i++) {
        fprintf(fp, "  %-4d %-20s %-8s %-10.1f %-8.2f %-10lld\n",
                i,
                g_model_table[i].model_name[0] ? g_model_table[i].model_name : "(未命名)",
                g_model_table[i].in_use ? "已加载" : "未加载",
                g_model_table[i].peak_mem_mb,
                g_model_table[i].last_latency_ms,
                g_model_table[i].access_count);
    }
    fprintf(fp, "\n");

    /* ---- 写入实际进程内存 ---- */
    fprintf(fp, "【三、实际进程内存（Linux /proc）】\n");
    fprintf(fp, "  当前 RSS:        %zu MB\n", rss_mb);
    /* ru.ru_maxrss 在 Linux 下单位是 KB，需除以 1024 转 MB */
    fprintf(fp, "  峰值 RSS:        %ld MB\n", ru.ru_maxrss / 1024);
    fprintf(fp, "\n");

    /* ---- 写入结论 ---- */
    fprintf(fp, "【四、结论】\n");
    if (g_current_used_mem > g_total_mem_limit * MEM_SIM_THRESHOLD_RATIO) {
        fprintf(fp, "  ⚠ 当前使用率超过 %.0f%% 阈值，建议卸载闲置模型\n",
                MEM_SIM_THRESHOLD_RATIO * 100);
    } else {
        fprintf(fp, "  ✓ 当前内存使用正常\n");
    }
    fprintf(fp, "=====================================================\n");

    pthread_mutex_unlock(&g_mem_mutex);

    /* 关闭文件，刷出缓冲区。
     * 不调用 fclose 会导致数据丢失（fprintf 先写入内存缓冲区，fclose 才刷盘）。 */
    fclose(fp);

    printf("[mem_sim] 性能报表已生成：%s\n", path);
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 7：mem_sim_record_inference —— 记录推理结果
 * -----------------------------------------------------------------------------
 * 【参数】model_id：模型 ID
 * 【参数】latency_ms：本轮推理延迟（毫秒）
 * 【返回值】0=成功，-1=失败
 * 【作用】
 *   更新模型的 last_latency_ms 和 access_count，供报表统计。
 *   每次推理调用后由 AI 引擎层或业务层调用。
 * -----------------------------------------------------------------------------
 */
int mem_sim_record_inference(int model_id, double latency_ms)
{
    if (model_id < 0 || model_id >= MEM_SIM_MAX_MODELS) {
        return -1;
    }
    if (g_initialized == 0) {
        return -1;
    }

    pthread_mutex_lock(&g_mem_mutex);

    /* 仅当模型已加载时才记录 */
    if (g_model_table[model_id].in_use == 0) {
        pthread_mutex_unlock(&g_mem_mutex);
        return -1;
    }

    /* 更新最近延迟 */
    g_model_table[model_id].last_latency_ms = latency_ms;
    /* 累计访问次数（用于闲置判定，访问越多越不闲置） */
    g_model_table[model_id].access_count++;
    /* 更新访问时间 */
    g_model_table[model_id].last_access_sec = get_current_time_sec();

    pthread_mutex_unlock(&g_mem_mutex);
    return 0;
}
