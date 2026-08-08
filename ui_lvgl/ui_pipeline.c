/*
 * =============================================================================
 * EdgeSim UI 管道桥接实现文件  ui_pipeline.c
 * =============================================================================
 * 【文件作用】
 *   实现 ui_pipeline.h 声明的接口：
 *     1. 注册 LVGL timer 周期性轮询管道
 *     2. 提供发送辅助函数，封装 TaskData 构造
 *     3. 收到数据后分发到注册的回调
 *
 * 【关键 API 速查（来自 multi_proc）】
 *   - proc_send(target, &task)   ：非阻塞发送，>0=成功
 *   - proc_recv(source, &task, 0)：超时 0 立即返回，>0=收到数据
 *   - TaskData 字段：cmd / data_len / model_id / timestamp / data_buf[4096]
 *
 * 【线程模型】
 *   全部在 UI 主线程（LVGL 线程）执行，无多线程冲突。
 *   LVGL timer 回调中循环 proc_recv 直到返回 -2（无数据）。
 * =============================================================================
 */

#include "ui_pipeline.h"
#include "multi_proc.h"
#include "hardware_sim.h"   /* 阶段 2：mem_sim_alloc/free/get_usage/get_model_mb */
#include "sqlite_db.h"      /* 阶段 3：perf_log_add 性能日志入库 */
#include <stdio.h>
#include <stdlib.h>         /* 阶段 2：atoi 解析子进程上报的内存大小 */
#include <string.h>
#include <time.h>

/* ---- 内部状态：业务层注册的 4 个回调函数 ---- */
static ui_on_llm_reply_cb  g_llm_cb  = NULL;
static ui_on_ocr_result_cb g_ocr_cb  = NULL;
static ui_on_asr_result_cb g_asr_cb  = NULL;
static ui_on_mem_update_cb g_mem_cb  = NULL;

/* LVGL timer 句柄，用于销毁时删除 */
static lv_timer_t *g_poll_timer = NULL;

/* 阶段 2：内存监控定时刷新 timer 句柄 */
static lv_timer_t *g_mem_timer = NULL;

/* 轮询周期：50ms（够快不掉消息，又不太占 CPU）
 * LVGL timer 周期单位是毫秒 */
#define PIPELINE_POLL_PERIOD_MS 50

/* 阶段 2：内存监控刷新周期（毫秒）
 * 【阶段 4.5 性能调整：300ms → 1000ms】
 * 背景：用户反馈"点击按钮就卡"。内存面板每 300ms 重绘进度条
 *       （含动画）+ 60 点趋势曲线 + 标签，是常驻的周期性重绘开销，
 *       与按钮点击触发的重绘叠加后感知为卡顿。
 * 调整：1 秒刷新对内存监控完全足够（内存变化非毫秒级），
 *       重绘频率降为原来的 1/3，常驻开销大幅削减。
 *       如需更平滑可改 500ms（权衡流畅度与实时性）。 */
#define MEM_REFRESH_PERIOD_MS 1000

/* proc_id → model_id 映射偏移：
 * PROC_ID_LLM=1 → MODEL_ID_LLM=0，即 model_id = source - 1 */
#define PROC_TO_MODEL(src)  ((src) - PROC_ID_LLM)


/*
 * -----------------------------------------------------------------------------
 * fill_task —— 内部辅助函数：填充 TaskData 通用字段
 * 【参数】task     ：待填充的 TaskData 指针
 * 【参数】cmd      ：命令码（task_cmd_t）
 * 【参数】model_id ：模型 ID（PROC_ID_* 与 MODEL_ID_* 对应，直接复用）
 * 【参数】data     ：负载数据（可为 NULL，仅 cmd 时用）
 * 【说明】调用者需提前 memset(task, 0, sizeof(*task))
 * -----------------------------------------------------------------------------
 */
static void fill_task(TaskData *task, int cmd, int model_id, const char *data)
{
    task->cmd       = cmd;
    task->model_id  = model_id;
    task->reserved  = 0;
    /* time(NULL) 返回 Unix 时间戳（秒），用于日志排序与超时判定 */
    task->timestamp = (long long)time(NULL);
    if (data != NULL) {
        /* strncpy 防止溢出：data_buf 大小 TASK_DATA_MAX=4096
         * 注意 strncpy 不会自动补 '\0'，需手动确保 */
        size_t len = strlen(data);
        if (len >= TASK_DATA_MAX) {
            len = TASK_DATA_MAX - 1;
        }
        memcpy(task->data_buf, data, len);
        task->data_buf[len] = '\0';
        task->data_len = (int)len;
    } else {
        task->data_len = 0;
    }
}


/*
 * -----------------------------------------------------------------------------
 * dispatch_task —— 内部辅助函数：根据 TaskData.cmd 分发到回调
 * 【参数】source：来源进程 ID
 * 【参数】task  ：收到的 TaskData 指针
 * 【说明】确保 data_buf 以 '\0' 结尾后再传给回调，避免越界读取
 * -----------------------------------------------------------------------------
 */
static void dispatch_task(int source, const TaskData *task)
{
    /* 防御性：保证 data_buf 是合法 C 字符串 */
    char buf[TASK_DATA_MAX + 1];
    int len = task->data_len;
    if (len < 0 || len > TASK_DATA_MAX) {
        len = 0;
    }
    memcpy(buf, task->data_buf, len);
    buf[len] = '\0';

    /* ---- 阶段 2：处理内存仿真状态上报 ----
     * 子进程在模型加载成功/卸载时回发 TASK_CMD_MODEL_LOADED / UNLOADED，
     * 父进程在此统一调用 mem_sim_alloc / mem_sim_free，维护全局内存视图。
     * 处理完直接 return，不再进入下方按来源的结果分发。 */
    if (task->cmd == TASK_CMD_MODEL_LOADED ||
        task->cmd == TASK_CMD_MODEL_UNLOADED) {
        int model_id = PROC_TO_MODEL(source);   /* 1→0, 2→1, 3→2 */
        if (model_id < 0 || model_id >= MODEL_ID_MAX) {
            printf("[ui_pipeline] 内存上报的进程来源非法: %d\n", source);
            return;
        }
        if (task->cmd == TASK_CMD_MODEL_LOADED) {
            /* data_buf 携带该模型占用 MB（子进程用 snprintf 写的小数文本） */
            double mb = atof(buf);
            if (mb <= 0.0) {
                /* 防御：未解析到合法大小，给一个默认值，避免 alloc 拒绝 */
                mb = 50.0;
            }
            /* 阶段 3：给模型设置真实名称（性能报表显示用，替代 "unknown"）。
             * 注意 model_id 在上面已校验 ∈[0, MODEL_ID_MAX)，数组索引安全。 */
            static const char *const g_model_names[MODEL_ID_MAX] = {
                "Qwen2.5-1.5B",   /* MODEL_ID_LLM */
                "PP-OCRv3",       /* MODEL_ID_OCR */
                "Whisper-Base"    /* MODEL_ID_ASR */
            };
            mem_sim_set_model_name(model_id, g_model_names[model_id]);
            /* 登记占用：若超过 90% 阈值，mem_sim_alloc 内部会自动回收闲置模型 */
            mem_sim_alloc(mb, model_id);
        } else {
            /* 模型卸载：释放内存仿真登记（未加载时返回 -1，可忽略） */
            mem_sim_free(model_id);
        }
        return;
    }

    /* ---- 阶段 2：处理推理完成事件（动态监控）----
     * 子进程每完成一次推理，上报 TASK_CMD_INFER_DONE（data_buf=耗时 ms）。
     * 父进程调用 mem_sim_record_inference 更新该模型：
     *   - access_count +1、last_access 刷新 → "最闲置模型"判定动态化，
     *     内存回收跟随真实使用频率而非加载顺序；
     *   - last_latency_ms 更新 → 后续性能报表（阶段 3）直接复用。
     * 处理完直接 return。 */
    if (task->cmd == TASK_CMD_INFER_DONE) {
        int model_id = PROC_TO_MODEL(source);   /* 1→0, 2→1, 3→2 */
        double latency = atof(buf);             /* 解析耗时文本为 double */
        if (model_id >= 0 && model_id < MODEL_ID_MAX) {
            mem_sim_record_inference(model_id, latency);
            /* 阶段 3：写入性能日志表（perf_log）
             * mem_peak 取当前仿真已用内存作为参考（单位 MB） */
            mem_usage_t usage;                  /* 当前内存快照 */
            size_t peak_mb = 0;                 /* 作为峰值的参考值 */
            if (mem_sim_get_usage(&usage) == 0) {
                peak_mb = (size_t)usage.used_mb;
            }
            perf_log_add(model_id, latency, peak_mb);
        }
        return;
    }

    /* ---- 命令白名单：只分发"推理结果"消息（治本防御）----
     * 背景：main.c 的 child_send_result 复用 TASK_CMD_INFER(=2) 作为
     *       结果消息标识（详见 main.c child_send_result 注释）。
     * 策略：非 INFER 命令一律丢弃。即使未来新增命令码时漏掉拦截，
     *       任何控制类消息（INIT/PING/EXIT/DESTROY 等）也绝不会
     *       泄漏进结果回调（聊天窗/OCR/ASR 面板），杜绝错显。 */
    if (task->cmd != TASK_CMD_INFER) {
        return;
    }

    /* 按来源进程分发到对应回调 */
    switch (source) {
    case PROC_ID_LLM:
        /* LLM 子进程返回的是对话回复文本 */
        if (g_llm_cb) {
            g_llm_cb(buf);
        }
        break;

    case PROC_ID_OCR:
        /* OCR 子进程返回的是识别文字（可能多行） */
        if (g_ocr_cb) {
            g_ocr_cb(buf);
        }
        break;

    case PROC_ID_ASR:
        /* ASR 子进程返回的是转写文字 */
        if (g_asr_cb) {
            g_asr_cb(buf);
        }
        break;

    case PROC_ID_UI:
        /* 来自 UI 进程自身？理论上不会发生，忽略 */
        break;

    default:
        printf("[ui_pipeline] 未知来源: %d\n", source);
        break;
    }
}


/*
 * -----------------------------------------------------------------------------
 * mem_refresh_timer_cb —— 内存监控定时刷新回调（阶段 2 新增）
 * -----------------------------------------------------------------------------
 * 【参数】timer：LVGL timer 句柄（未使用）
 * 【作用】每 MEM_REFRESH_PERIOD_MS 毫秒被 LVGL 调用一次：
 *   1. 从 hardware_sim 读取全局使用快照（mem_sim_get_usage）
 *   2. 分别读取 LLM/OCR/ASR 三个模型的当前占用（mem_sim_get_model_mb）
 *   3. 调用 UI 注册的内存回调 g_mem_cb，驱动监控面板更新
 * 【设计说明】
 *   内存仿真数据由父进程统一维护（子进程上报 → mem_sim_alloc/free），
 *   因此这里读取的就是"仿真开发板"的真实状态，不再有 mock 数据。
 * -----------------------------------------------------------------------------
 */
static void mem_refresh_timer_cb(lv_timer_t *timer)
{
    mem_usage_t usage;              /* 内存使用快照 */
    double llm_mb, ocr_mb, asr_mb;  /* 三个模型各自占用（MB，支持小数） */

    (void)timer;

    /* 尚无 UI 回调注册：跳过本轮（例如 BUILD_EDGESIM=OFF 的测试场景） */
    if (g_mem_cb == NULL) {
        return;
    }

    /* 读取全局使用快照；仿真器未初始化时返回 -1，跳过本轮 */
    if (mem_sim_get_usage(&usage) != 0) {
        return;
    }

    /* 读取三个模型各自占用；-1.0 表示读取失败，按 0 处理 */
    llm_mb = mem_sim_get_model_mb(MODEL_ID_LLM);
    ocr_mb = mem_sim_get_model_mb(MODEL_ID_OCR);
    asr_mb = mem_sim_get_model_mb(MODEL_ID_ASR);
    if (llm_mb < 0.0) llm_mb = 0.0;
    if (ocr_mb < 0.0) ocr_mb = 0.0;
    if (asr_mb < 0.0) asr_mb = 0.0;

    /* 驱动 UI 面板：used_mb / total_limit_mb / 三模型占用（全 double） */
    g_mem_cb(usage.used_mb,
             usage.total_limit_mb,
             llm_mb, ocr_mb, asr_mb);
}


/*
 * -----------------------------------------------------------------------------
 * poll_timer_cb —— LVGL timer 回调函数
 * 【参数】timer：LVGL timer 句柄（未使用）
 * 【说明】每 PIPELINE_POLL_PERIOD_MS 毫秒被 LVGL 调用一次。
 *        循环 proc_recv 直到无数据，避免消息积压。
 *        UI 进程是父进程，需要轮询 3 个子进程的回管道。
 * -----------------------------------------------------------------------------
 */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;   /* 显式忽略未使用参数，消除编译警告 */

    TaskData task;
    int sources[] = { PROC_ID_LLM, PROC_ID_OCR, PROC_ID_ASR };
    int i;

    /* 遍历 3 个子进程来源，每个最多连续读 16 次（防止某一进程狂发卡死 UI） */
    for (i = 0; i < 3; i++) {
        int read_count = 0;
        while (read_count < 16) {
            /* proc_recv 参数：
             *   source     ：来源进程 ID
             *   &task      ：接收缓冲区
             *   0          ：超时 0=不等待立即返回
             * 返回值：>0=收到字节数，-1=错误，-2=超时(无数据)，-3=EOF(对端关闭) */
            int ret = proc_recv(sources[i], &task, 0);
            if (ret > 0) {
                dispatch_task(sources[i], &task);
                read_count++;
            } else if (ret == -3) {
                /* 对端关闭（子进程崩溃或退出），可在此触发 UI 告警
                 * 这里只打印日志，不阻塞 timer */
                printf("[ui_pipeline] 进程 %d 管道已关闭\n", sources[i]);
                break;
            } else {
                /* -1 错误 或 -2 无数据：跳出该来源循环 */
                break;
            }
        }
    }
}


/*
 * =============================================================================
 * 公共接口实现
 * =============================================================================
 */

void ui_pipeline_set_callbacks(ui_on_llm_reply_cb llm_cb,
                                ui_on_ocr_result_cb ocr_cb,
                                ui_on_asr_result_cb asr_cb,
                                ui_on_mem_update_cb mem_cb)
{
    /* 累加式注册：只更新非 NULL 的回调，避免后注册的模块覆盖先注册的
     * 例如 ui_chat_window 注册 LLM 回调后，ui_mem_monitor 注册 mem 回调，
     * 两者不会互相覆盖（之前直接赋值会导致 LLM 回调被清成 NULL） */
    if (llm_cb) g_llm_cb = llm_cb;
    if (ocr_cb) g_ocr_cb = ocr_cb;
    if (asr_cb) g_asr_cb = asr_cb;
    if (mem_cb) g_mem_cb = mem_cb;
}

int ui_pipeline_init(void)
{
    /* 创建 LVGL timer，周期 PIPELINE_POLL_PERIOD_MS，回调 poll_timer_cb
     * lv_timer_create 参数：
     *   callback ：定时回调函数指针
     *   period   ：周期（毫秒）
     *   user_data：用户数据指针（NULL=不传）
     * 返回 timer 句柄，NULL=失败 */
    g_poll_timer = lv_timer_create(poll_timer_cb, PIPELINE_POLL_PERIOD_MS, NULL);
    if (g_poll_timer == NULL) {
        printf("[ui_pipeline] LVGL timer 创建失败\n");
        return -1;
    }
    printf("[ui_pipeline] 已启动 %dms 管道轮询 timer\n",
           PIPELINE_POLL_PERIOD_MS);

    /* 阶段 2：创建内存监控定时刷新 timer。
     * 从 hardware_sim 读取真实内存数据驱动监控面板。
     * 创建失败不致命：面板保持初始状态，轮询功能不受影响。 */
    g_mem_timer = lv_timer_create(mem_refresh_timer_cb, MEM_REFRESH_PERIOD_MS, NULL);
    if (g_mem_timer == NULL) {
        printf("[ui_pipeline] 内存刷新 timer 创建失败\n");
    } else {
        printf("[ui_pipeline] 已启动 %dms 内存刷新 timer\n",
               MEM_REFRESH_PERIOD_MS);
    }
    return 0;
}

int ui_pipeline_send_infer(int target, const char *input)
{
    TaskData task;

    if (input == NULL) {
        return -1;
    }
    memset(&task, 0, sizeof(task));
    fill_task(&task, TASK_CMD_INFER, target, input);

    /* proc_send 返回值：>0=发送字节数，-1=错误，-2=超时 */
    int ret = proc_send(target, &task);
    if (ret < 0) {
        printf("[ui_pipeline] send_infer 失败 (target=%d, ret=%d)\n",
               target, ret);
        return -1;
    }
    return 0;
}

int ui_pipeline_send_init(int target, const char *model_path)
{
    TaskData task;

    if (model_path == NULL) {
        return -1;
    }
    memset(&task, 0, sizeof(task));
    fill_task(&task, TASK_CMD_INIT, target, model_path);

    if (proc_send(target, &task) < 0) {
        printf("[ui_pipeline] send_init 失败 (target=%d)\n", target);
        return -1;
    }
    return 0;
}

int ui_pipeline_send_destroy(int target)
{
    TaskData task;
    memset(&task, 0, sizeof(task));
    fill_task(&task, TASK_CMD_DESTROY, target, NULL);

    if (proc_send(target, &task) < 0) {
        printf("[ui_pipeline] send_destroy 失败 (target=%d)\n", target);
        return -1;
    }
    return 0;
}

int ui_pipeline_send_exit(void)
{
    TaskData task;
    int targets[] = { PROC_ID_LLM, PROC_ID_OCR, PROC_ID_ASR };
    int i;
    int fail = 0;

    /* 向 3 个子进程都发 EXIT，让它们优雅退出 */
    memset(&task, 0, sizeof(task));
    fill_task(&task, TASK_CMD_EXIT, 0, NULL);
    for (i = 0; i < 3; i++) {
        if (proc_send(targets[i], &task) < 0) {
            printf("[ui_pipeline] send_exit 失败 (target=%d)\n", targets[i]);
            fail++;
        }
    }
    return fail == 0 ? 0 : -1;
}


