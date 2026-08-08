#ifndef UI_PIPELINE_H
#define UI_PIPELINE_H

/*
 * =============================================================================
 * EdgeSim UI 管道桥接模块内部头文件  ui_pipeline.h
 * =============================================================================
 * 【文件作用】
 *   ui_pipeline 是 ui_lvgl 模块内部的桥接层，负责：
 *     1. 把用户操作（点按钮、输入文本）打包成 TaskData，调 proc_send 发给业务层
 *     2. 周期性调 proc_recv 轮询业务层返回，分发到对应回调
 *   本头文件仅供 ui_lvgl 内部各窗口 .c 文件包含，不对外公开。
 *
 * 【与 multi_proc 的关系】
 *   ui_pipeline 是 multi_proc 的调用者，不是替代品。
 *   proc_init() 在 main 中调用，本模块只用 proc_send / proc_recv。
 *
 * 【设计要点】
 *   - 发送：直接调 proc_send，非阻塞，超时 1 秒
 *   - 接收：在 LVGL timer 中以 50ms 周期轮询，超时 0 立即返回
 *   - 分发：根据 TaskData.cmd 调用对应回调，UI 不解析业务字段
 * =============================================================================
 */

#include "ui_lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif


/*
 * ui_pipeline_init —— 初始化管道桥接（注册 LVGL timer）
 * 【返回值】0=成功，-1=失败
 * 【说明】在 ui_init 内部调用，创建 50ms 周期的 LVGL timer，
 *         timer 回调中循环调 proc_recv 直到无数据（防止积压）。
 */
int ui_pipeline_init(void);

/*
 * ui_pipeline_send_infer —— 发送推理任务给业务层
 * 【参数】target：目标进程（PROC_ID_LLM / PROC_ID_OCR / PROC_ID_ASR）
 * 【参数】input：输入文本（用户问题 / 图片路径 / 音频路径）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部填充 TaskData.cmd=TASK_CMD_INFER, data_buf=input, data_len=strlen
 */
int ui_pipeline_send_infer(int target, const char *input);

/*
 * ui_pipeline_send_init —— 发送模型加载指令给业务层
 * 【参数】target：目标进程
 * 【参数】model_path：模型文件路径
 * 【返回值】0=成功，-1=失败
 */
int ui_pipeline_send_init(int target, const char *model_path);

/*
 * ui_pipeline_send_destroy —— 发送模型卸载指令给业务层
 * 【参数】target：目标进程
 * 【返回值】0=成功，-1=失败
 */
int ui_pipeline_send_destroy(int target);

/*
 * ui_pipeline_send_exit —— 发送进程退出指令（关闭子进程）
 * 【返回值】0=成功，-1=失败
 * 【说明】调用方在程序退出前调用，让 3 个子进程优雅退出
 */
int ui_pipeline_send_exit(void);


#ifdef __cplusplus
}
#endif

#endif /* UI_PIPELINE_H */
