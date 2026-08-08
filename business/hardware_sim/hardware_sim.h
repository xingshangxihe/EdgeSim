#ifndef HARDWARE_SIM_H
#define HARDWARE_SIM_H

/*
 * =============================================================================
 * EdgeSim 内存仿真模块公共头文件  hardware_sim.h
 * =============================================================================
 * 【文件作用】
 *   声明 hardware_sim 模块对外提供的所有数据类型与函数接口。
 *   其它层（如 multi_proc、ui_lvgl）只能通过本头文件调用内存仿真功能，
 *   严禁直接访问 hardware_sim.c 内部的 static 全局变量。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.1 节「hardware_sim（内存仿真）」
 *
 * 【模块职责】
 *   - 模拟低成本开发板（T153/RV1106）的小内存环境
 *   - 全局内存上限管控（256MB / 512MB / 1GB 可配置）
 *   - 各 AI 模型内存占用实时统计
 *   - 接近阈值（90%）自动卸载闲置模型，防止 OOM 崩溃
 *   - 生成性能报表（模型体积、推理延迟、内存峰值）
 * =============================================================================
 */

#include <stddef.h>   /* size_t 类型定义所在头文件 */

#ifdef __cplusplus
extern "C" {
#endif
/* 上面这个 extern "C" 块的作用：
 *   当本头文件被 C++ 源文件 #include 时，告诉 C++ 编译器：
 *   "这些函数是用 C 编译器编译的，链接时请用 C 的名称修饰规则"。
 *   否则 C++ 会做 name mangling（名称重写），导致链接找不到符号。
 *   这是 C/C++ 混合编程的标准做法，新手务必记住。
 */


/* =============================================================================
 * 一、模型 ID 枚举
 * =============================================================================
 * 【作用】标识三类 AI 模型，作为 mem_sim_alloc/free 等接口的 model_id 参数。
 * 【易错点】MODEL_ID_MAX 仅作边界标记，不可作为有效 model_id 传入任何接口。
 *           所有遍历模型的循环都应写成 i < MODEL_ID_MAX。
 * ========================================================================== */
typedef enum {
    MODEL_ID_LLM = 0,   /* 大语言模型（llama.cpp / Qwen1.8B），通常占用内存最大 */
    MODEL_ID_OCR = 1,   /* OCR 文字识别（RKNN Lite），占用内存中等 */
    MODEL_ID_ASR = 2,   /* 语音识别（whisper.cpp），占用内存中等 */
    MODEL_ID_MAX = 3    /* 模型总数边界标记（不可作为有效 ID 使用） */
} model_id_t;


/* =============================================================================
 * 二、单模型内存信息结构体
 * =============================================================================
 * 【作用】记录一个 AI 模型在内存仿真器中的全部状态。
 *         本结构体通过 g_model_table[MODEL_ID_MAX] 数组在 .c 内部维护，
 *         外部通过接口间接访问，不可直接读写。
 * ========================================================================== */
typedef struct {
    int       in_use;            /* 占用标志：0=该槽位空闲，1=已加载模型 */
    int       model_id;          /* 模型 ID（与枚举值对应，便于调试打印） */
    double    alloc_mb;          /* 当前已申请内存（单位 MB，支持小数，如 200.5） */
    long long last_access_sec;   /* 最近一次访问的 Unix 时间戳（秒），用于闲置判定 */
    long long access_count;      /* 累计访问次数，访问越少越"闲置" */
    char      model_name[32];    /* 模型名称字符串（如 "Qwen1.8B-INT8"），用于报表 */
    double    last_latency_ms;   /* 最近一次推理延迟（毫秒），用于报表 */
    double    peak_mem_mb;       /* 该模型历史峰值内存（MB），支持小数，用于报表 */
} model_mem_info_t;


/* =============================================================================
 * 三、内存使用快照结构体
 * =============================================================================
 * 【作用】mem_sim_get_usage() 接口的输出参数类型，封装当前内存使用概况。
 *         外部调用者声明一个局部变量，把地址传入，接口填充后即可读取。
 * ========================================================================== */
typedef struct {
    double total_limit_mb;  /* 用户设置的全局内存上限（MB） */
    double used_mb;         /* 当前已用内存（MB） */
    double free_mb;         /* 剩余可用内存（MB） = total_limit_mb - used_mb */
    double usage_percent;   /* 使用率百分比（0~100），超过 90 触发自动回收 */
} mem_usage_t;


/* =============================================================================
 * 四、对外接口声明（详见 hardware_sim.c 中每个函数的逐行注释）
 * =============================================================================
 * 返回值约定：成功返回 0，失败返回 -1（除非另有说明）。
 * ========================================================================== */

/*
 * mem_sim_init —— 初始化内存仿真器
 * 参数 limit_mb：全局内存上限（推荐 256 / 512 / 2048，支持小数）
 * 返回值：0=成功，-1=失败（参数非法）
 */
int mem_sim_init(double limit_mb);

/*
 * mem_sim_alloc —— 为指定模型申请内存（256MB 阈值限制核心接口）
 * 参数 size_mb：要申请的内存大小（MB，支持小数，如 200.5）
 * 参数 model_id：模型 ID（MODEL_ID_LLM / OCR / ASR）
 * 返回值：0=成功，-1=失败（超限且无法回收）
 * 说明：若申请后使用率将超过 90%，会先自动触发 mem_sim_unload_idle() 回收闲置模型。
 */
int mem_sim_alloc(double size_mb, int model_id);

/*
 * mem_sim_free —— 释放指定模型的内存
 * 参数 model_id：要释放的模型 ID
 * 返回值：0=成功，-1=失败（该模型未加载）
 */
int mem_sim_free(int model_id);

/*
 * mem_sim_get_usage —— 获取当前内存使用快照（内存读取接口）
 * 参数 usage：输出参数，调用者传入 mem_usage_t 变量地址
 * 返回值：0=成功，-1=失败（参数为空）
 */
int mem_sim_get_usage(mem_usage_t *usage);

/*
 * mem_sim_get_model_mb —— 获取指定模型当前占用内存（MB）
 * 参数 model_id：模型 ID（MODEL_ID_LLM / OCR / ASR）
 * 返回值：>=0.0=当前占用 MB（该模型未加载时为 0），-1.0=失败（参数非法或未初始化）
 * 说明：阶段 2 新增。供 UI 层内存监控面板按模型分别显示占用，
 *       无需暴露内部模型表，只返回该模型 alloc_mb 一个数值。
 */
double mem_sim_get_model_mb(int model_id);

/*
 * mem_sim_set_model_name —— 设置指定模型的显示名称（阶段 3 新增）
 * 参数 model_id：模型 ID（MODEL_ID_LLM / OCR / ASR）
 * 参数 name：模型名称字符串（如 "Qwen2.5-1.5B"），最长 31 字符
 * 返回值：0=成功，-1=失败（参数非法）
 * 说明：名称写入模型表 model_name 字段，供性能报表（mem_sim_gen_report）
 *       显示真实模型名，不再显示默认 "unknown"。名称在模型卸载后保留。
 */
int mem_sim_set_model_name(int model_id, const char *name);

/*
 * mem_sim_unload_idle —— 卸载最闲置模型，回收内存（闲置模型释放接口）
 * 返回值：>=0=被卸载的 model_id，-1=无可卸载模型
 * 说明：闲置度 = access_count 越少越闲置；并列时 last_access_sec 越早越闲置。
 */
int mem_sim_unload_idle(void);

/*
 * mem_sim_gen_report —— 生成性能报表到文件（性能报表接口）
 * 参数 path：报表文件输出路径（如 "perf_report.txt"）
 * 返回值：0=成功，-1=失败（文件创建失败）
 */
int mem_sim_gen_report(const char *path);

/*
 * mem_sim_record_inference —— 记录一次推理结果（供报表统计）
 * 参数 model_id：模型 ID
 * 参数 latency_ms：本轮推理延迟（毫秒）
 * 返回值：0=成功，-1=失败
 */
int mem_sim_record_inference(int model_id, double latency_ms);


#ifdef __cplusplus
} /* extern "C" 结束 */
#endif

#endif /* HARDWARE_SIM_H */
