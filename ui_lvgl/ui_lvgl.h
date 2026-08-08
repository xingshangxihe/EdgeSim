#ifndef UI_LVGL_H
#define UI_LVGL_H

/*
 * =============================================================================
 * EdgeSim UI 表现层公共头文件  ui_lvgl.h
 * =============================================================================
 * 【文件作用】
 *   声明 ui_lvgl 模块对外提供的所有接口。
 *   本模块是项目表现层，仅负责：
 *     1. LVGL 图形界面显示（主对话窗口、文件导入面板、内存监控图表、悬浮小窗口）
 *     2. 接收用户输入（键盘、鼠标、按钮）
 *     3. 通过管道（multi_proc 模块）与业务层通信
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 2.1 节「架构分层」
 *   EdgeSim_Design.md 第 3.5 节「ui_lvgl（界面）」
 *
 * 【设计约束（严格遵守）】
 *   1. 仅界面逻辑，禁止任何 AI 推理代码（不直接调用 ai_engine）
 *   2. 仅界面逻辑，禁止任何内存管理代码（不直接调用 hardware_sim）
 *   3. 与业务层通信只能通过管道（multi_proc 模块）
 *   4. ARM64 交叉编译时定义 NO_SDL，跳过 SDL2 初始化（开发板/Termux 无桌面）
 *
 * 【数据流向（典型流程）】
 *   用户点击发送按钮
 *      → ui_chat_window 收集文本
 *      → ui_pipeline 调 proc_send 发 TaskData 给 LLM 子进程
 *      → 业务层调用 llm_engine_run 推理
 *      → 子进程 proc_send 把结果回管道
 *      → ui_pipeline 在 LVGL timer 中 proc_recv
 *      → ui_chat_window 把回复追加到消息列表显示
 * =============================================================================
 */

#include "lvgl.h"   /* LVGL v8+ 主头文件，提供 lv_obj_t / lv_timer_t 等类型 */

#ifdef __cplusplus
extern "C" {
#endif


/* =============================================================================
 * UI 主题色定义（浅色主题，集中管理改一处生效）
 * =============================================================================
 * 【设计参考】类似微信/Notion 的浅色风格
 *   - 白色背景 + 深色文字 + 蓝色强调色
 *   - 圆角 8~12px，扁平化，无渐变
 * 【使用方式】各 .c 文件用 lv_color_hex(UI_COLOR_xxx) 设置控件颜色
 * ========================================================================== */
#define UI_COLOR_BG            0xFFFFFF   /* 主背景：纯白 */
#define UI_COLOR_BG_SECOND     0xF7F8FA   /* 次背景：浅灰（卡片/面板） */
#define UI_COLOR_BG_HOVER      0xEFF1F5   /* 悬停反馈：更深一点 */
#define UI_COLOR_BORDER        0xE5E7EB   /* 边框：浅灰 */
#define UI_COLOR_TEXT          0x1F2937   /* 主文字：深灰黑 */
#define UI_COLOR_TEXT_SECOND   0x6B7280   /* 次文字：中灰 */
#define UI_COLOR_TEXT_WHITE    0xFFFFFF   /* 反白文字 */
#define UI_COLOR_PRIMARY       0x3B82F6   /* 强调色：蓝（主按钮） */
#define UI_COLOR_PRIMARY_DARK  0x2563EB   /* 强调色按下/悬停：深蓝 */
#define UI_COLOR_USER_BUBBLE   0x3B82F6   /* 用户气泡背景：蓝 */
#define UI_COLOR_AI_BUBBLE     0xF3F4F6   /* AI 气泡背景：浅灰 */
#define UI_COLOR_SUCCESS       0x10B981   /* 成功：绿 */
#define UI_COLOR_WARNING       0xF59E0B   /* 警告：橙 */
#define UI_COLOR_DANGER        0xEF4444   /* 危险：红 */
#define UI_COLOR_FLOAT_BG      0x1F2937   /* 悬浮窗背景：深灰（半透明） */


/* =============================================================================
 * UI 字体定义
 * =============================================================================
 * UI_FONT_DEFAULT：默认字体，用 LVGL 内置 montserrat_14（英文/数字/符号）
 * UI_FONT_CHINESE：中文字体，由 lv_font_conv 生成（含 ASCII + GB2312 常用字）
 *                  仅用于需要显示中文的控件（如 ASR 转写结果）
 * ========================================================================== */
#define UI_FONT_DEFAULT  (&lv_font_montserrat_14)

#ifdef HAS_CHINESE_FONT
    LV_FONT_DECLARE(lv_font_chinese_16);
    #define UI_FONT_CHINESE  (&lv_font_chinese_16)
#else
    #define UI_FONT_CHINESE  (&lv_font_montserrat_14)
#endif


/* =============================================================================
 * 一、整体初始化与主循环
 * =============================================================================
 * 【典型使用流程】
 *   ui_init(NULL, NULL);    // 初始化（内部完成 SDL2 + LVGL + 各窗口创建）
 *   while (1) {
 *       ui_loop();          // 主循环（处理 SDL 事件 + LVGL tick + 管道轮询）
 *       usleep(5000);
 *   }
 *   ui_deinit();
 * ========================================================================== */

/*
 * ui_init —— 初始化整个 UI 模块
 * 【参数】argc：命令行参数个数（用于 SDL2 初始化，可为 0）
 * 【参数】argv：命令行参数数组（可为 NULL）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部依次完成：
 *          1. SDL2 显示驱动初始化（NO_SDL 模式下跳过）
 *          2. LVGL 核心初始化（lv_init）
 *          3. 显示缓冲区创建（lv_disp_draw_buf_init）
 *          4. 显示驱动注册（lv_disp_drv_register）
 *          5. 鼠标驱动注册（SDL2 鼠标事件转 LVGL）
 *          6. 创建 4 个窗口：对话 / 文件导入 / 内存监控 / 悬浮窗
 *          7. 启动管道轮询 timer（每 50ms 调用 ui_pipeline_poll）
 */
int ui_init(int argc, char **argv);

/*
 * ui_loop —— 执行一次 UI 主循环迭代
 * 【返回值】0=继续运行，-1=收到退出事件（应跳出主循环）
 * 【说明】必须由调用者周期性调用，每次：
 *          1. 处理 SDL2 事件（NO_SDL 下跳过）
 *          2. 调用 lv_timer_handler 推进 LVGL 任务
 *          3. 调用 lv_tick_inc 更新 LVGL 时基
 *          4. （管道轮询由 LVGL timer 内部自动完成）
 */
int ui_loop(void);

/*
 * ui_deinit —— 释放 UI 模块资源
 * 【说明】销毁所有 LVGL 控件、关闭 SDL2 窗口、释放显示缓冲区
 */
void ui_deinit(void);


/*
 * ui_chat_window_scroll —— 滚动消息列表（供鼠标滚轮事件调用）
 * 【参数】dy：滚动像素数（正值=向下滚动，负值=向上滚动）
 */
void ui_chat_window_scroll(int32_t dy);


/*
 * ui_clipboard_copy —— 复制文本到系统剪贴板
 * 【参数】text：要复制的文本
 */
void ui_clipboard_copy(const char *text);


/*
 * ui_chat_window_paste —— 粘贴文本到输入框（供 Ctrl+V 调用）
 * 【参数】text：要粘贴的文本
 */
void ui_chat_window_paste(const char *text);


/*
 * ui_chat_window_handle_char —— 直接处理一个按键（绕过 LVGL indev 周期）
 * 【参数】c：Unicode 码点（普通字符）或 LV_KEY_* 常量（特殊键）
 */
void ui_chat_window_handle_char(uint32_t c);


/*
 * ui_chat_window_set_ocr_image_path —— 记录最近一次 OCR 识别的图片路径
 * 【参数】path：图片文件路径（NULL 表示清空记录）
 * 【说明】ui_file_import 在发送 OCR 任务前调用此函数记录图片路径，
 *         on_ocr_result_cb 回调触发时读取该路径，把图片路径存入 kb_files 表。
 *         由于 ui_pipeline 回调签名只传识别文本不传原始路径，需用此接口桥接。
 */
void ui_chat_window_set_ocr_image_path(const char *path);


/*
 * 文件导入面板的输入分发接口（供 ui_main 的 SDL 键盘事件调用）
 * 【背景】ui_main 的键盘/粘贴事件原本只发给聊天窗口，文件导入面板的
 *         路径输入框收不到输入。这三个接口让 ui_main 按面板可见性分发。
 */
int  ui_file_import_is_visible(void);                       /* 查询面板是否可见 */
void ui_file_import_handle_char(uint32_t c);                /* 向路径输入框注入按键 */
void ui_file_import_paste(const char *text);                /* 粘贴文本到路径输入框 */


/*
 * 录音功能（SDL2 音频采集，供 ASR 引擎使用）
 */
int  ui_main_start_recording(void);          /* 开始录音 */
void ui_main_poll_recording(void);           /* ui_loop 中轮询读取音频 */
int  ui_main_stop_recording(const char *path); /* 停止录音，保存 WAV */
int  ui_main_is_recording(void);             /* 查询录音状态 */

/*
 * 截图功能（SDL2 渲染器像素读取，供 OCR 引擎使用）
 * 【参数】path：保存路径（BMP 格式）
 * 【返回值】0=成功，-1=失败
 * 【说明】读取 SDL 渲染器当前画面，保存为 24 位 BMP 文件
 */
int  ui_main_take_screenshot(const char *path);

/* =============================================================================
 * 二、对外回调（业务层调用 UI 显示结果）
 * =============================================================================
 * 【设计原因】
 *   ui_pipeline 在 LVGL timer 中收到管道消息后，需要更新界面显示。
 *   各窗口模块向 ui_pipeline 注册回调函数，实现解耦：
 *     - ui_chat_window 注册 on_llm_reply 回调，收到 LLM 回复时刷新消息列表
 *     - ui_mem_monitor 注册 on_mem_update 回调，收到内存数据时刷新进度条
 *   这样 ui_pipeline 不需要知道具体窗口控件，只需调用回调。
 * ========================================================================== */

/*
 * ui_on_llm_reply_cb —— LLM 回复到达时的回调函数类型
 * 【参数】text：LLM 回复文本（以 '\0' 结尾）
 */
typedef void (*ui_on_llm_reply_cb)(const char *text);

/*
 * ui_on_ocr_result_cb —— OCR 识别结果到达时的回调函数类型
 * 【参数】text：识别到的文字（多行用 '\n' 分隔）
 */
typedef void (*ui_on_ocr_result_cb)(const char *text);

/*
 * ui_on_asr_result_cb —— ASR 转写结果到达时的回调函数类型
 * 【参数】text：转写文字
 */
typedef void (*ui_on_asr_result_cb)(const char *text);

/*
 * ui_on_mem_update_cb —— 内存使用更新回调函数类型
 * 【参数】used_mb：已用内存（MB）
 * 【参数】total_mb：内存上限（MB）
 * 【参数】llm_mb/ocr_mb/asr_mb：三个模型各自占用（MB）
 * 【阶段 2 浮点化】参数由 int 升级为 double：占用值带 1 位小数
 * （如 LLM 200.5MB），UI 显示真实体积而非整数。
 */
typedef void (*ui_on_mem_update_cb)(double used_mb, double total_mb,
                                     double llm_mb, double ocr_mb, double asr_mb);

/*
 * ui_pipeline_set_callbacks —— 业务层向 UI 注册回调
 * 【说明】UI 模块内部各窗口在 init 时调用此函数注册回调。
 *        传入 NULL 表示不接收该类消息。
 */
void ui_pipeline_set_callbacks(ui_on_llm_reply_cb llm_cb,
                                ui_on_ocr_result_cb ocr_cb,
                                ui_on_asr_result_cb asr_cb,
                                ui_on_mem_update_cb mem_cb);


#ifdef __cplusplus
}
#endif

#endif /* UI_LVGL_H */
