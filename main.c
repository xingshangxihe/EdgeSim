/*
 * =============================================================================
 * EdgeSim 主程序入口  main.c
 * =============================================================================
 * 【文件作用】
 *   EdgeSim 可执行文件 edgesim 的主入口。编排 4 进程架构：
 *     1. 注册子进程入口回调
 *     2. proc_init() fork 出 LLM/OCR/ASR 3 个子进程
 *     3. 父进程（UI）：ui_init → ui_loop 主循环 → ui_deinit
 *     4. 子进程：进入 child_main 工作循环，收任务→调引擎→回结果
 *
 * 【4 进程拓扑】
 *   [UI 父进程] ──pipe──→ [LLM 子进程] (调 llm_engine)
 *                ──pipe──→ [OCR 子进程] (调 ocr_engine)
 *                ──pipe──→ [ASR 子进程] (调 asr_engine)
 *   每条管道双向（父→子 + 子→父），TaskData 为统一消息格式。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 1.3.1 节「4 进程隔离架构」
 *   EdgeSim_Design.md 第 3.2 节「multi_proc」
 *
 * 【子进程工作循环模式】
 *   while (1) {
 *       proc_recv(PROC_ID_UI, &task, 1000);  // 等 1 秒
 *       switch (task.cmd) {
 *           TASK_CMD_INIT   → xxx_engine_init(model_path)
 *           TASK_CMD_INFER  → xxx_engine_run(input, output) → proc_send 回复
 *           TASK_CMD_EXIT   → 跳出循环 → destroy → exit
 *       }
 *   }
 *
 * 【引擎说明】
 *   引擎层支持两套实现（编译开关切换，见各引擎目录）：
 *     - Mock 桩模式：init/run/destroy 立即返回，用于验证进程与 UI 全链路
 *     - 真实模式（HAS_LLAMA_CPP / HAS_WHISPER_CPP / HAS_ONNXRUNTIME）：
 *       真正调用第三方推理库。子进程工作循环逻辑对两种模式完全一致，
 *       切换引擎时本文件无需修改。
 * =============================================================================
 */

#include "multi_proc.h"   /* proc_init / proc_send / proc_recv / TaskData */
#include "ui_lvgl.h"      /* ui_init / ui_loop / ui_deinit */
#include "llm_engine.h"   /* llm_engine_init / run / destroy */
#include "ocr_engine.h"   /* ocr_engine_init / run / destroy */
#include "asr_engine.h"   /* asr_engine_init / run / destroy */
#include "sqlite_db.h"    /* db_init / db_close 持久化对话与知识库 */
#include "hardware_sim.h" /* mem_sim_init/alloc/free 硬件内存仿真（阶段 2） */

#include <stdio.h>        /* printf */
#include <stdlib.h>       /* exit */
#include <string.h>       /* memset / memcpy / strlen */
#include <unistd.h>       /* usleep / getpid */
#include <time.h>         /* time */
#include <sys/stat.h>     /* mkdir 创建 data 目录 */
#include <sys/types.h>    /* mode_t */
#include <sys/resource.h> /* setpriority：AI 子进程降优先级（阶段 4.5） */
#include <errno.h>        /* errno / EEXIST 判断目录是否已存在 */


/* =============================================================================
 * 一、默认模型路径（相对可执行文件的工作目录）
 * =============================================================================
 * 【说明】
 *   - Mock 模式下：引擎 init 不真正加载文件，路径仅作占位，不存在也能"成功"
 *   - 真实模式下：需确保模型文件存在于运行目录下的相对路径
 *
 * 【路径约定（与项目 models/ 目录一致）】
 *   models/llm/qwen2.5-1.5b-instruct-q4_k_m.gguf  —— LLM 量化模型
 *   models/ocr/det.onnx                            —— OCR 检测模型
 *   models/asr/ggml-base-q8_0.bin                  —— ASR 语音模型
 * ========================================================================== */
#define DEFAULT_LLM_MODEL  "models/llm/qwen2.5-1.5b-instruct-q4_k_m.gguf"
#define DEFAULT_OCR_MODEL  "models/ocr/"
#define DEFAULT_ASR_MODEL  "models/asr/ggml-base-q8_0.bin"

/* 数据库路径（相对运行目录 build/，实际生成在项目根目录 data/ 下）
 * 程序在 build/ 目录运行，../data 即项目根目录的 data 文件夹 */
#define DB_DATA_DIR  "../data"
#define DB_PATH      "../data/edgesim.db"


/* =============================================================================
 * 阶段 2 新增：硬件内存仿真相关常量与映射表
 * =============================================================================
 * 【设计说明】
 *   内存仿真器由父进程（UI 主进程）统一维护：mem_sim_init 在父进程调用，
 *   子进程通过 fork 继承初始状态；子进程每次加载/卸载模型时，
 *   用 TASK_CMD_MODEL_LOADED / UNLOADED 回发父进程，
 *   父进程（ui_pipeline）据此调用 mem_sim_alloc / mem_sim_free。
 *   这样整个"仿真开发板"的内存视图只有一个来源，UI 面板显示即真实数据。
 *
 * 【预设占用值】
 *   MEM_SIM_LLM/OCR/ASR_MB 模拟低端开发板上小型量化模型的占用：
 *     LLM 200MB + OCR 50MB + ASR 80MB = 330MB > 256MB 上限，
 *   因此全部加载时会触发 mem_sim 的 90% 阈值自动回收闲置模型，
 *   可直观演示"OOM 保护自动卸载"特性。
 * ========================================================================== */
/* 【真实数据化改造（阶段 2 用户要求）】
 * 1. MEM_SIM_LIMIT_MB 由 512 提升到 2048，原因：
 *      模型加载时改为上报"真实模型文件体积"（stat 获取字节→MB）：
 *        LLM  gguf 文件 ≈ 985MB
 *        OCR  PP-OCR 三 onnx 之和 ≈ 60MB
 *        ASR  ggml-base ≈ 78MB
 *      三模型真实合计 ≈ 1124MB，原 512MB 上限会互相回收甚至拒绝加载，
 *      2048MB 上限下 1124 < 2048×0.9=1843，真实三模型可共存。
 * 2. 原预设值 200.5/50.3/80.2 保留为 fallback：当 stat 失败
 *    （mock 模式/文件缺失）时使用，保证面板仍有合理数据。 */
#define MEM_SIM_LIMIT_MB    2048.0 /* 仿真器全局内存上限（MB），容纳真实模型体积 */
#define MEM_SIM_LLM_MB      200.5 /* LLM 模型预设占用（MB，带小数贴近真实体积） */
#define MEM_SIM_OCR_MB       50.3 /* OCR 模型预设占用（MB） */
#define MEM_SIM_ASR_MB       80.2 /* ASR 模型预设占用（MB） */

/* proc_id → 模型占用(MB) 映射表：索引=proc_id，0 号是 UI 占位。
 * 子进程上报时直接用 self_id 索引，父进程端用 (source-1) 还原为 model_id。
 * 类型为 double：占用值带 1 位小数，UI 面板显示更贴近真实模型体积
 * （不再像旧版全是整数 200/50/80）。 */
static const double g_proc_mem_mb[PROC_ID_MAX] = {
    0.0,               /* PROC_ID_UI 占位（父进程不加载模型） */
    MEM_SIM_LLM_MB,    /* PROC_ID_LLM = 1 */
    MEM_SIM_OCR_MB,    /* PROC_ID_OCR = 2 */
    MEM_SIM_ASR_MB     /* PROC_ID_ASR = 3 */
};


/* =============================================================================
 * 二、常量定义
 * ========================================================================== */

/* 推理输出缓冲区大小（与 llm_engine.h 注释的 ENGINE_OUTPUT_MAX 一致）
 * 注意：TaskData.data_buf 只有 4096 字节，超长回复会在发送时被截断 */
#define ENGINE_OUTPUT_MAX  8192

/* 子进程 proc_recv 超时：1 秒
 * 太短会忙等浪费 CPU，太长会延迟响应 EXIT 命令 */
#define CHILD_RECV_TIMEOUT_MS  1000

/* 父进程退出前给子进程的优雅退出等待时间（毫秒） */
#define PARENT_EXIT_GRACE_MS   200


/* =============================================================================
 * 三、子进程内部辅助函数
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：child_send_result
 * -----------------------------------------------------------------------------
 * 【作用】子进程把推理结果通过管道发回 UI 父进程
 * 【参数】text：结果文本（以 '\0' 结尾的 C 字符串）
 * 【说明】填充 TaskData.cmd=TASK_CMD_INFER（复用作为"推理结果"标识），
 *         超过 TASK_DATA_MAX-1 的部分截断，调 proc_send(PROC_ID_UI, &task)。
 *         发送失败只打日志，不退出子进程（避免一次发送失败导致整个子进程挂掉）。
 * -----------------------------------------------------------------------------
 */
static void child_send_result(const char *text)
{
    TaskData task;          /* 待发送的消息结构体 */
    size_t   len;           /* 文本长度（不含 '\0'） */

    /* 清零整个结构体（含 reserved 等字段） */
    memset(&task, 0, sizeof(task));

    /* 复用 TASK_CMD_INFER 作为"推理结果"标识
     * UI 端的 ui_pipeline dispatch_task 会根据 source 分发到对应回调，
     * 不依赖 cmd 字段区分结果类型，所以这里复用 INFER 即可 */
    task.cmd       = TASK_CMD_INFER;
    task.model_id  = 0;
    task.timestamp = (long long)time(NULL);

    /* 截断到 TASK_DATA_MAX-1，留 1 字节给 '\0' */
    len = strlen(text);
    if (len >= TASK_DATA_MAX) {
        len = TASK_DATA_MAX - 1;
    }
    memcpy(task.data_buf, text, len);
    task.data_buf[len] = '\0';
    task.data_len = (int)len;

    /* proc_send 返回值：>0=发送字节数，-1=错误，-2=超时
     * 这里只打日志不做重试，避免阻塞子进程主循环 */
    if (proc_send(PROC_ID_UI, &task) < 0) {
        printf("[child] 发送结果失败 (len=%d)\n", (int)len);
    }
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：file_size_mb
 * -----------------------------------------------------------------------------
 * 【作用】用 stat 获取单文件真实体积（字节），换算成 MB（double）
 * 【参数】path：文件路径
 * 【返回值】>=0.0=文件体积（MB）；-1.0=失败（路径为空/文件不存在）
 * 【说明】阶段 2 真实数据化改造：内存监控显示真实模型体积而非预设假值。
 * -----------------------------------------------------------------------------
 */
static double file_size_mb(const char *path)
{
    struct stat st;             /* stat 结果：保存文件大小等信息 */

    if (path == NULL || stat(path, &st) != 0) {
        return -1.0;            /* 文件不存在或不可访问 */
    }
    /* 字节 → MB：除以 1024×1024，结果保留小数 */
    return (double)st.st_size / (1024.0 * 1024.0);
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：model_occupied_mb
 * -----------------------------------------------------------------------------
 * 【作用】计算某进程对应模型的真实占用体积（MB）
 * 【参数】self_id    ：子进程身份（PROC_ID_LLM / OCR / ASR）
 * 【参数】model_path ：模型路径。LLM/ASR 为单个文件；OCR 为目录
 * 【返回值】>=0.0=真实体积（MB）；-1.0=无法获取（路径无效）
 * 【设计说明】
 *   - LLM / ASR：模型是单个文件，直接 stat 文件大小
 *   - OCR：模型是目录（models/ocr/），目录 stat 的大小不是内容之和，
 *     因此对 PP-OCR 的 det/cls/rec 三个 onnx 分别 stat 后求和
 *   - 真实体积让内存监控数据"真实有用"，不再是预设假值
 * -----------------------------------------------------------------------------
 */
static double model_occupied_mb(proc_id_t self_id, const char *model_path)
{
    char full[1024];            /* 拼接出的子模型完整路径 */
    double total = 0.0;         /* 累计体积（MB） */
    double mb;                  /* 单个文件体积（MB） */

    if (model_path == NULL || model_path[0] == '\0') {
        return -1.0;
    }

    if (self_id == PROC_ID_OCR) {
        /* OCR 目录：三个子模型文件体积求和 */
        snprintf(full, sizeof(full), "%s/det.onnx", model_path);
        mb = file_size_mb(full);
        if (mb < 0.0) return -1.0;      /* det.onnx 缺失，整体判定失败 */
        total += mb;
        snprintf(full, sizeof(full), "%s/cls.onnx", model_path);
        mb = file_size_mb(full);
        if (mb < 0.0) return -1.0;
        total += mb;
        snprintf(full, sizeof(full), "%s/rec.onnx", model_path);
        mb = file_size_mb(full);
        if (mb < 0.0) return -1.0;
        total += mb;
        return total;
    }

    /* LLM / ASR：单个模型文件 */
    return file_size_mb(model_path);
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：child_notify_mem_status
 * -----------------------------------------------------------------------------
 * 【作用】子进程向父进程上报模型加载/卸载状态（阶段 2 新增）
 * 【参数】self_id    ：子进程身份（PROC_ID_LLM / OCR / ASR）
 * 【参数】loaded     ：1=模型加载成功，0=模型已卸载/加载失败
 * 【参数】model_path ：模型路径（LLM/ASR 单文件、OCR 目录）。loaded=0 时传 NULL
 * 【说明】
 *   父进程侧由 ui_pipeline 接收本消息并调用 mem_sim_alloc/free，
 *   从而让"仿真开发板"的内存视图与真实模型加载状态保持一致。
 *   消息格式：cmd=MODEL_LOADED/UNLOADED，model_id=self_id（proc 编号），
 *             loaded 时 data_buf 携带该模型占用 MB（文本形式）。
 *   【真实数据化】loaded 时优先用 stat 读取模型文件真实体积（MB），
 *   stat 失败（mock 模式/文件缺失）时回退到预设值 g_proc_mem_mb。
 *   发送失败只打日志，不阻塞子进程主循环（与 child_send_result 一致）。
 * -----------------------------------------------------------------------------
 */
static void child_notify_mem_status(proc_id_t self_id, int loaded,
                                    const char *model_path)
{
    TaskData task;              /* 待发送的消息结构体 */
    double mb;                  /* 该模型占用（MB，真实体积或预设回退值） */

    memset(&task, 0, sizeof(task));
    /* loaded 决定命令码：加载成功 vs 卸载/失败 */
    task.cmd       = loaded ? TASK_CMD_MODEL_LOADED : TASK_CMD_MODEL_UNLOADED;
    /* 直接用 proc_id 编号（1/2/3），父进程端用 (source-1) 还原为 model_id */
    task.model_id  = (int)self_id;
    task.timestamp = (long long)time(NULL);

    if (loaded && self_id >= 0 && self_id < PROC_ID_MAX) {
        /* 1. 优先：真实模型文件体积（stat，MB 带小数） */
        mb = model_occupied_mb(self_id, model_path);
        /* 2. 回退：stat 失败时使用预设值（见 g_proc_mem_mb 注释） */
        if (mb <= 0.0) {
            mb = g_proc_mem_mb[self_id];
        }
        /* 把占用 MB 转成文本（保留 1 位小数）放入 data_buf，父进程用 atof 解析 */
        snprintf(task.data_buf, sizeof(task.data_buf), "%.1f", mb);
        task.data_len = (int)strlen(task.data_buf);
    } else {
        /* 卸载消息不需要载荷 */
        task.data_len = 0;
    }

    if (proc_send(PROC_ID_UI, &task) < 0) {
        printf("[child %d] 通知内存状态失败 (loaded=%d)\n", self_id, loaded);
    }
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：child_notify_infer_done
 * -----------------------------------------------------------------------------
 * 【作用】子进程每次推理完成后向父进程上报耗时（阶段 2 动态监控新增）
 * 【参数】self_id   ：子进程身份（PROC_ID_LLM / OCR / ASR）
 * 【参数】latency_ms：本次推理耗时（毫秒，double）
 * 【说明】
 *   父进程（ui_pipeline）收到本消息后调用 mem_sim_record_inference，
 *   效果：
 *     1. 模型 access_count +1、last_access 刷新 → 内存仿真器据此动态
 *        调整"最闲置模型"判定，卸载决策跟随真实使用频率；
 *     2. last_latency_ms 被更新 → 后续性能报表（阶段 3）直接可用。
 *   这就是"动态监控"的数据来源：面板刷新（ui_pipeline 定时器）+
 *   推理事件上报（本函数）两条链路共同驱动。
 * -----------------------------------------------------------------------------
 */
static void child_notify_infer_done(proc_id_t self_id, double latency_ms)
{
    TaskData task;              /* 待发送的消息结构体 */

    memset(&task, 0, sizeof(task));
    task.cmd       = TASK_CMD_INFER_DONE;
    /* 用 proc_id 编号（1/2/3），父进程端用 (source-1) 还原为 model_id */
    task.model_id  = (int)self_id;
    task.timestamp = (long long)time(NULL);
    /* 耗时毫秒转文本（保留 2 位小数），父进程用 atof 解析 */
    snprintf(task.data_buf, sizeof(task.data_buf), "%.2f", latency_ms);
    task.data_len  = (int)strlen(task.data_buf);

    if (proc_send(PROC_ID_UI, &task) < 0) {
        printf("[child %d] 通知推理完成失败\n", self_id);
    }
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：engine_init_by_id
 * -----------------------------------------------------------------------------
 * 【作用】根据子进程 ID 调用对应引擎的 init 函数
 * 【参数】self_id     ：子进程身份（PROC_ID_LLM / OCR / ASR）
 * 【参数】model_path  ：模型文件路径
 * 【返回值】0=成功，-1=失败（或未知 self_id）
 * 【说明】集中分发，避免在主循环里写多处 switch
 * -----------------------------------------------------------------------------
 */
static int engine_init_by_id(proc_id_t self_id, const char *model_path)
{
    switch (self_id) {
    case PROC_ID_LLM:
        return llm_engine_init(model_path);
    case PROC_ID_OCR:
        return ocr_engine_init(model_path);
    case PROC_ID_ASR:
        return asr_engine_init(model_path);
    default:
        return -1;
    }
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：engine_run_by_id
 * -----------------------------------------------------------------------------
 * 【作用】根据子进程 ID 调用对应引擎的 run 函数
 * 【参数】self_id ：子进程身份
 * 【参数】input  ：输入文本（问题/图片路径/音频路径）
 * 【参数】output ：输出缓冲区，至少 ENGINE_OUTPUT_MAX 字节
 * 【返回值】0=成功，-1=失败
 * -----------------------------------------------------------------------------
 */
static int engine_run_by_id(proc_id_t self_id, const char *input, char *output)
{
    switch (self_id) {
    case PROC_ID_LLM:
        return llm_engine_run(input, output);
    case PROC_ID_OCR:
        return ocr_engine_run(input, output);
    case PROC_ID_ASR:
        return asr_engine_run(input, output);
    default:
        return -1;
    }
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：engine_destroy_by_id
 * -----------------------------------------------------------------------------
 * 【作用】根据子进程 ID 调用对应引擎的 destroy 函数
 * 【说明】destroy 内部已处理"未加载"情况，可安全重复调用
 * -----------------------------------------------------------------------------
 */
static void engine_destroy_by_id(proc_id_t self_id)
{
    switch (self_id) {
    case PROC_ID_LLM:
        llm_engine_destroy();
        break;
    case PROC_ID_OCR:
        ocr_engine_destroy();
        break;
    case PROC_ID_ASR:
        asr_engine_destroy();
        break;
    default:
        break;
    }
}


/* =============================================================================
 * 四、子进程工作循环
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：child_worker_loop
 * -----------------------------------------------------------------------------
 * 【作用】子进程通用工作循环（LLM/OCR/ASR 共用）
 * 【参数】self_id：当前子进程身份
 * 【流程】
 *   1. 选择默认模型路径
 *   2. while(1) 循环 proc_recv 等待父进程任务
 *   3. 按 task.cmd 分发：
 *        INIT    → 加载模型（先卸载旧的）
 *        INFER   → 若未加载则自动加载默认模型 → 推理 → 发结果回 UI
 *        DESTROY → 卸载模型
 *        EXIT    → 跳出循环
 *   4. 循环退出后确保模型已卸载
 * 【设计要点】
 *   - INFER 时若模型未加载，自动用默认模型加载（Mock 模式立即成功）
 *     这样 UI 端无需显式发 INIT 即可直接发问题，降低集成复杂度
 *   - 父进程管道关闭（ret==-3）时子进程主动退出，避免僵尸
 * -----------------------------------------------------------------------------
 */
static void child_worker_loop(proc_id_t self_id)
{
    TaskData    task;                  /* 接收任务缓冲区 */
    char        output[ENGINE_OUTPUT_MAX];  /* 推理输出缓冲区 */
    int         model_loaded = 0;      /* 模型是否已加载标志 */
    const char *default_model = NULL;  /* 默认模型路径 */
    int         ret;                   /* 通用返回值 */

    /* 根据 self_id 选择默认模型路径 */
    switch (self_id) {
    case PROC_ID_LLM:
        default_model = DEFAULT_LLM_MODEL;
        break;
    case PROC_ID_OCR:
        default_model = DEFAULT_OCR_MODEL;
        break;
    case PROC_ID_ASR:
        default_model = DEFAULT_ASR_MODEL;
        break;
    default:
        printf("[child %d] 未知进程 ID，退出\n", self_id);
        return;
    }

    printf("[child %d] 子进程启动，PID=%d，默认模型=%s\n",
           self_id, (int)getpid(), default_model);

    /* ---- 主循环 ---- */
    while (1) {
        /* 从父进程接收任务，超时 CHILD_RECV_TIMEOUT_MS 毫秒
         * 返回值：>0=收到字节数，-1=错误，-2=超时(无数据)，-3=EOF(父进程关闭管道) */
        ret = proc_recv(PROC_ID_UI, &task, CHILD_RECV_TIMEOUT_MS);

        if (ret == -3) {
            /* 父进程管道关闭（通常父进程已退出），子进程也退出 */
            printf("[child %d] 父进程管道已关闭，退出\n", self_id);
            break;
        }
        if (ret <= 0) {
            /* -1 错误 或 -2 超时：继续循环等待 */
            continue;
        }

        /* 保证 data_buf 是合法 C 字符串（防御性编程） */
        if (task.data_len < 0 || task.data_len >= TASK_DATA_MAX) {
            task.data_len = 0;
        }
        task.data_buf[task.data_len] = '\0';

        /* ---- 按 cmd 分发 ---- */
        switch (task.cmd) {

        case TASK_CMD_INIT: {
            /* 加载模型命令
             * 优先用 task.data_buf 传来的路径，为空则用默认路径 */
            const char *model_path = task.data_buf;
            if (model_path[0] == '\0') {
                model_path = default_model;
            }
            printf("[child %d] 收到 INIT，加载模型: %s\n", self_id, model_path);

            /* 先卸载旧模型（避免内存泄漏） */
            if (model_loaded) {
                engine_destroy_by_id(self_id);
                model_loaded = 0;
                /* 卸载旧模型后同步通知父进程更新内存视图（卸载无需路径） */
                child_notify_mem_status(self_id, 0, NULL);
            }

            /* 调对应引擎 init */
            ret = engine_init_by_id(self_id, model_path);
            if (ret == 0) {
                model_loaded = 1;
                printf("[child %d] 模型加载成功\n", self_id);
                child_send_result("[System] Model loaded successfully");
                /* 阶段 2：模型加载成功，通知父进程登记占用。
                 * 传 model_path：父进程端 stat 真实体积后登记 */
                child_notify_mem_status(self_id, 1, model_path);
            } else {
                printf("[child %d] 模型加载失败\n", self_id);
                child_send_result("[System] Model load failed");
                /* 加载失败：确保父进程侧该模型处于未加载状态 */
                child_notify_mem_status(self_id, 0, NULL);
            }
            break;
        }

        case TASK_CMD_INFER: {
            /* 推理命令：task.data_buf 是输入（问题/图片路径/音频路径） */
            const char *input = task.data_buf;
            printf("[child %d] 收到 INFER，输入: %.40s%s\n",
                   self_id, input, (strlen(input) > 40 ? "..." : ""));

            /* 若模型未加载，自动用默认模型加载
             * Mock 模式下 init 立即返回 0，不影响流程 */
            if (!model_loaded) {
                printf("[child %d] 模型未加载，自动加载默认模型\n", self_id);
                if (engine_init_by_id(self_id, default_model) == 0) {
                    model_loaded = 1;
                    /* 阶段 2：自动加载成功后同样上报（传默认模型路径，父进程 stat 真实体积） */
                    child_notify_mem_status(self_id, 1, default_model);
                } else {
                    child_send_result("[System] Auto model load failed, cannot infer");
                    /* 加载失败：父进程侧保持未加载状态 */
                    child_notify_mem_status(self_id, 0, NULL);
                    break;
                }
            }

            /* 调对应引擎 run，并用单调时钟测量推理耗时
             * CLOCK_MONOTONIC 不受系统时间调整影响，适合测耗时 */
            output[0] = '\0';
            struct timespec ts_beg, ts_end;
            clock_gettime(CLOCK_MONOTONIC, &ts_beg);
            ret = engine_run_by_id(self_id, input, output);
            clock_gettime(CLOCK_MONOTONIC, &ts_end);
            /* 秒差 ×1000 + 纳秒差 /1e6 = 毫秒 */
            double latency_ms =
                (double)(ts_end.tv_sec  - ts_beg.tv_sec)  * 1000.0 +
                (double)(ts_end.tv_nsec - ts_beg.tv_nsec) / 1000000.0;

            if (ret == 0 && output[0] != '\0') {
                /* 推理成功：把结果发回 UI */
                child_send_result(output);
            } else {
                child_send_result("[System] Inference failed or no result");
            }

            /* 阶段 2：上报推理完成事件（含耗时），父进程更新该模型活跃度，
             * 让"闲置模型自动卸载"按真实使用频率动态决策 */
            child_notify_infer_done(self_id, latency_ms);
            break;
        }

        case TASK_CMD_DESTROY:
            /* 卸载模型命令 */
            printf("[child %d] 收到 DESTROY，卸载模型\n", self_id);
            if (model_loaded) {
                engine_destroy_by_id(self_id);
                model_loaded = 0;
                /* 阶段 2：模型已卸载，通知父进程释放内存仿真登记 */
                child_notify_mem_status(self_id, 0, NULL);
            }
            break;

        case TASK_CMD_EXIT:
            /* 退出命令 */
            printf("[child %d] 收到 EXIT，退出\n", self_id);
            goto cleanup;

        case TASK_CMD_PING:
            /* 心跳请求：回 PONG（用于后续健康检查） */
            printf("[child %d] 收到 PING，回 PONG\n", self_id);
            {
                TaskData pong;
                memset(&pong, 0, sizeof(pong));
                pong.cmd       = TASK_CMD_PONG;
                pong.timestamp = (long long)time(NULL);
                proc_send(PROC_ID_UI, &pong);
            }
            break;

        default:
            printf("[child %d] 未知 cmd=%d，忽略\n", self_id, task.cmd);
            break;
        }
    }

cleanup:
    /* 循环退出前确保模型已卸载，避免资源泄漏 */
    if (model_loaded) {
        engine_destroy_by_id(self_id);
        /* 阶段 2：子进程退出前上报卸载，父进程内存视图同步清零 */
        child_notify_mem_status(self_id, 0, NULL);
    }
    printf("[child %d] 子进程退出，PID=%d\n", self_id, (int)getpid());
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：child_main
 * -----------------------------------------------------------------------------
 * 【作用】子进程入口回调（proc_init fork 后子进程调用）
 * 【参数】self_id：子进程身份（PROC_ID_LLM / OCR / ASR）
 * 【说明】proc_child_fn 类型要求返回 void。子进程应在此函数内工作并最终 exit。
 *         本函数调 child_worker_loop 后立即 exit(0)，保证子进程不返回 main。
 * -----------------------------------------------------------------------------
 */
static void child_main(proc_id_t self_id)
{
    /* ---- 阶段 4.5 性能优化：AI 子进程降调度优先级 ----
     * 背景：用户在虚拟机上实测"启动初期（模型加载）/语音测试多时
     *       （ASR 推理）所有按钮卡顿，文字对话多不卡"。
     *       根因是 CPU 资源竞争：LLM/ASR 子进程的模型加载与推理
     *       会长期占满一个 CPU 核，虚拟机核数少时与 UI 主进程抢
     *       时间片，导致按钮响应延迟。
     * 方案：子进程 nice +10（降低调度优先级）。Linux 的 CFS 调度器
     *       保证高优先级（低 nice 值）进程优先获得 CPU——
     *       UI 主进程（默认 nice 0）在子进程（nice 10）之前被调度，
     *       按钮点击立即响应。推理略慢（低优先级）但发生在后台，
     *       用户感知为"等回复"，不影响交互流畅度。
     * 注意：setpriority 失败（如非 root 但 nice 值在允许范围内一般
     *       成功）不致命，仅记录日志，不影响功能。 */
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, 10) != 0 && errno != 0) {
        printf("[child %d] 设置低优先级失败: %s（可忽略）\n",
               self_id, strerror(errno));
    }

    child_worker_loop(self_id);
    /* 工作循环返回后退出子进程（正常退出码 0） */
    exit(0);
}


/* =============================================================================
 * 五、父进程（UI 主进程）入口
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：main
 * -----------------------------------------------------------------------------
 * 【作用】EdgeSim 主入口，运行在 UI 父进程中
 * 【流程】
 *   1. 注册子进程入口回调（必须在 proc_init 之前）
 *   2. proc_init fork 3 个子进程（子进程在 proc_init 内调 child_main 不返回）
 *   3. 父进程调 ui_init 初始化 SDL2 + LVGL + 各窗口 + 管道轮询 timer
 *   4. 父进程进入 ui_loop 主循环（5ms 一次），收到退出事件跳出
 *   5. 通知子进程 EXIT，等待 200ms，ui_deinit + proc_cleanup
 * -----------------------------------------------------------------------------
 */
int main(int argc, char **argv)
{
    int ret;
    int self;

    printf("============================================================\n");
    printf("  EdgeSim 离线 AI 助手启动\n");
    printf("  架构: 4 进程隔离 (UI + LLM + OCR + ASR)\n");
    printf("============================================================\n");

    /* ---- 1. 注册子进程入口回调 ----
     * 必须在 proc_init 之前调用，否则 fork 后子进程无回调可执行
     * proc_set_child_handler 内部只是把函数指针存到全局变量 */
    proc_set_child_handler(child_main);

    /* ---- 2. fork 3 个子进程 ----
     * 返回值：
     *   PROC_ID_UI(0) = 父进程，继续往下执行
     *   1~3           = 子进程，但子进程会在 proc_init 内部调用 child_main，
     *                   正常情况下不会返回到这里
     *   -1            = fork 失败 */
    self = proc_init();
    if (self < 0) {
        printf("[main] proc_init 失败，退出\n");
        return -1;
    }

    if (self != PROC_ID_UI) {
        /* 子进程不应该走到这里（child_main 内部已 exit）
         * 作为安全网，防止 child_main 意外返回导致子进程继续执行 main */
        printf("[main] 子进程 %d 意外返回 main，退出\n", self);
        return 0;
    }

    /* ---- 3. 父进程：初始化数据库 ----
     * 仅父进程（UI 进程）操作数据库，子进程不涉及。
     * 数据库文件放在项目根目录 data/ 下（相对 build/ 运行目录是 ../data/）。
     * 先确保 data 目录存在（mkdir 返回 -1 且 errno=EEXIST 表示已存在，不算错误）。
     * db_init 内部会 CREATE TABLE IF NOT EXISTS，可安全重复调用。 */
    if (mkdir(DB_DATA_DIR, 0755) == -1 && errno != EEXIST) {
        printf("[main] 创建数据目录失败: %s (errno=%d)\n", DB_DATA_DIR, errno);
        /* 目录创建失败不致命，db_init 仍会尝试打开，失败再处理 */
    }
    if (db_init(DB_PATH) != 0) {
        printf("[main] 数据库初始化失败: %s\n", DB_PATH);
        /* 数据库失败不阻止程序运行，OCR 结果仅不持久化，UI 仍可用 */
    } else {
        printf("[main] 数据库已就绪: %s\n", DB_PATH);
    }

    /* ---- 3.5 父进程：初始化硬件内存仿真器 ----
     * 阶段 2 新增。仿真器状态仅由父进程维护：
     *   - 父进程调用 mem_sim_init 设置内存上限（子进程 fork 时继承）
     *   - 子进程加载/卸载模型时通过管道上报，
     *     ui_pipeline 在父进程侧调用 mem_sim_alloc/free 更新
     *   - UI 内存监控面板定时从 mem_sim_get_usage 读取真实数据
     * 失败不致命：面板显示默认值，程序照常运行。 */
    if (mem_sim_init(MEM_SIM_LIMIT_MB) != 0) {
        printf("[main] 内存仿真器初始化失败（使用默认 512MB）\n");
    } else {
        /* 注意：MEM_SIM_LIMIT_MB 是 double，打印必须用 %f 系列
         * （用 %d 会读取 double 的位模式，输出垃圾值） */
        printf("[main] 内存仿真器就绪，上限 %.0f MB\n", MEM_SIM_LIMIT_MB);
    }

    /* ---- 4. 父进程：初始化 UI ----
     * ui_init 内部完成：
     *   SDL2 窗口创建 → LVGL 核心初始化 → 显示驱动注册 →
     *   4 个窗口创建（chat/file_import/mem_monitor/floating）→
     *   ui_pipeline_init 启动 50ms 管道轮询 timer */
    printf("[main] 父进程启动 UI，PID=%d\n", (int)getpid());
    ret = ui_init(argc, argv);
    if (ret != 0) {
        printf("[main] UI 初始化失败\n");
        db_close();
        proc_cleanup();
        return -1;
    }

    /* ---- 5. 父进程：UI 主循环 ----
     * ui_loop 每次迭代：
     *   处理 SDL2 事件 → lv_timer_handler（含管道轮询）→ lv_tick_inc
     * 返回 -1 表示收到退出事件（ESC 或窗口关闭按钮） */
    printf("[main] 进入 UI 主循环（按 ESC 或点关闭按钮退出）\n");
    while (1) {
        if (ui_loop() < 0) {
            break;
        }
        /* 5ms 休眠，避免 100% CPU
         * LVGL timer 内部会处理管道轮询（50ms 周期） */
        usleep(5000);
    }

    /* ---- 6. 父进程：优雅退出 ----
     * 顺序很重要：
     *   ① 先发 EXIT 给 3 个子进程，让它们 destroy 模型并退出
     *   ② 等待 200ms 让子进程完成清理
     *   ③ ui_deinit 销毁 SDL2/LVGL 资源
     *   ④ db_close 关闭数据库（flush 未写入数据到磁盘）
     *   ⑤ proc_cleanup 回收僵尸子进程、关闭管道 */
    printf("[main] 通知子进程退出...\n");
    ui_pipeline_send_exit();

    /* 给子进程一点时间优雅退出（destroy 模型可能耗时） */
    usleep(PARENT_EXIT_GRACE_MS * 1000);

    printf("[main] 清理 UI 与子进程资源...\n");
    ui_deinit();

    /* ---- 6.5 阶段 3：生成性能报表 ----
     * 汇总本次运行的内存仿真统计（各模型占用/延迟/峰值）+ 推理记录，
     * 写入运行目录下的 perf_report.txt。失败不致命，仅提示。 */
    if (mem_sim_gen_report("perf_report.txt") == 0) {
        printf("[main] 性能报表已生成: %s\n", "perf_report.txt");
    } else {
        printf("[main] 性能报表生成失败\n");
    }

    db_close();
    proc_cleanup();

    printf("[main] EdgeSim 已退出\n");
    return 0;
}
