/*
 * =============================================================================
 * EdgeSim UI 桌面悬浮小窗口实现  ui_floating_window.c
 * =============================================================================
 * 【文件作用】
 *   创建桌面悬浮快捷小窗口，包含：
 *     1. 显示/隐藏主窗口切换按钮
 *     2. 一键开始录音按钮（发给 ASR 子进程）
 *     3. 导入图片 OCR 按钮（打开文件导入面板，发给 OCR 子进程）
 *     4. 退出按钮
 *   对应设计文档 1.1.5 节「桌面悬浮快捷小窗口」。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 1.1.5 节
 *   EdgeSim_Design.md 第 3.5 节「ui_lvgl（界面）」
 *
 * 【LVGL 控件树】
 *   lv_obj_t *float_win (悬浮窗, 右上角, 半透明)
 *   ├── lv_obj_t *btn_hide    ("⌂" 隐藏/显示主窗)
 *   ├── lv_obj_t *btn_mic     ("🎤" 录音)
 *   ├── lv_obj_t *btn_ocr     ("📷" 导入图片 OCR)
 *   └── lv_obj_t *btn_exit    ("✕" 退出)
 *
 * 【拖拽实现】
 *   悬浮窗可拖动：监听 LV_EVENT_PRESSING 事件，调用 lv_obj_set_pos
 *   跟随鼠标位置移动。
 * =============================================================================
 */

#include "ui_lvgl.h"
#include "ui_pipeline.h"
#include "multi_proc.h"
#include <stdio.h>

/* ---- 模块内部状态 ---- */
static lv_obj_t *g_float_win = NULL;   /* 悬浮窗主容器 */
static int       g_main_visible = 1;    /* 主窗口当前是否可见（1=可见） */

/* 录音状态：0=未录音, 1=录音中（仅 UI 标记，真实状态由 ASR 子进程维护） */
static int       g_recording = 0;


/* 声明外部函数：主窗口显示/隐藏由 ui_main 提供
 * 这里用 extern 引用，避免循环依赖
 * ui_main.c 中实现 ui_main_window_show/hide */
extern void ui_main_window_show(void);
extern void ui_main_window_hide(void);
extern int  ui_main_request_exit(void);

/* 声明外部函数：文件导入面板显示由 ui_file_import 提供
 * OCR 按钮改为打开导入面板，让用户选择本地图片做识别 */
extern int ui_file_import_show(void);


/*
 * -----------------------------------------------------------------------------
 * float_win_drag_cb —— 悬浮窗拖动事件回调
 * 【参数】e：LVGL 事件对象
 * 【说明】当悬浮窗被按下并移动时（LV_EVENT_PRESSING），
 *         让悬浮窗跟随鼠标位置。
 * -----------------------------------------------------------------------------
 */
static void float_win_drag_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_get_act();   /* 获取当前活动的输入设备（鼠标） */
    lv_point_t vect;

    if (indev == NULL) {
        return;
    }

    /* lv_indev_get_vect 获取鼠标自上次事件以来的位移向量 */
    lv_indev_get_vect(indev, &vect);

    /* 取当前位置并加上位移 */
    lv_coord_t x = lv_obj_get_x(obj) + vect.x;
    lv_coord_t y = lv_obj_get_y(obj) + vect.y;

    /* 限制不超出屏幕：通过 lv_obj_get_parent 获取父容器尺寸
     * 这里简化处理，不做严格边界检查（LVGL 默认会裁剪） */
    lv_obj_set_pos(obj, x, y);
}


/*
 * -----------------------------------------------------------------------------
 * btn_hide_cb —— 隐藏/显示主窗口按钮回调
 * -----------------------------------------------------------------------------
 */
static void btn_hide_cb(lv_event_t *e)
{
    (void)e;
    if (g_main_visible) {
        ui_main_window_hide();
        g_main_visible = 0;
    } else {
        ui_main_window_show();
        g_main_visible = 1;
    }
}


/*
 * -----------------------------------------------------------------------------
 * btn_mic_cb —— 录音按钮回调
 * 【说明】点击切换录音状态：
 *         - 开始录音：发 TASK_CMD_INIT 给 ASR 让它准备
 *         - 停止录音：发 TASK_CMD_INFER 触发转写
 *   真实项目需要更复杂的状态机，这里简化演示。
 * -----------------------------------------------------------------------------
 */
static void btn_mic_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);

    if (!g_recording) {
        /* 开始录音：调用 SDL2 音频采集 */
        if (ui_main_start_recording() == 0) {
            g_recording = 1;
            if (label) {
                lv_label_set_text(label, "Stop");
            }
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF5555), 0);
            printf("[ui_floating] 开始录音\n");
        } else {
            printf("[ui_floating] 录音设备打开失败\n");
        }
    } else {
        /* 停止录音：保存 WAV 文件，发送路径给 ASR 子进程 */
        g_recording = 0;
        if (label) {
            lv_label_set_text(label, "Rec");
        }
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x5588FF), 0);

        /* 停止录音并保存到 /tmp/edgesim_voice.wav */
        if (ui_main_stop_recording("/tmp/edgesim_voice.wav") == 0) {
            /* 发送 WAV 文件路径给 ASR 子进程 */
            if (ui_pipeline_send_infer(PROC_ID_ASR, "/tmp/edgesim_voice.wav") != 0) {
                printf("[ui_floating] 发送 ASR 任务失败\n");
            }
            printf("[ui_floating] 已停止录音，发送给 ASR\n");
        } else {
            printf("[ui_floating] 录音保存失败\n");
        }
    }
}


/*
 * -----------------------------------------------------------------------------
 * btn_ocr_cb —— 导入图片 OCR 按钮回调
 * 【说明】点击后打开文件导入面板，让用户输入本地图片路径：
 *   1. 用户在面板中选择 "Image (OCR)" 类型并填入图片路径
 *   2. 点击 Import 后，ui_file_import 把图片路径发给 OCR 子进程
 *   3. OCR 识别结果通过 on_ocr_result_cb 回调显示在聊天窗口并存入数据库
 * 【设计变更】原为截图 OCR（截取 UI 自身画面），但 UI 截图无实际文字
 *             意义不大，改为导入用户指定的真实图片做识别。
 * -----------------------------------------------------------------------------
 */
static void btn_ocr_cb(lv_event_t *e)
{
    (void)e;
    /* 打开文件导入面板，用户在其中填写图片路径并选择 Image 类型 */
    if (ui_file_import_show() != 0) {
        printf("[ui_floating] 文件导入面板显示失败\n");
        return;
    }
    printf("[ui_floating] 已打开图片导入面板\n");
}


/*
 * -----------------------------------------------------------------------------
 * btn_exit_cb —— 退出按钮回调
 * -----------------------------------------------------------------------------
 */
static void btn_exit_cb(lv_event_t *e)
{
    (void)e;
    /* 通过 ui_main_request_exit 通知主循环退出 */
    ui_main_request_exit();
}


/*
 * =============================================================================
 * 公共接口：ui_floating_window_create
 * =============================================================================
 * 【参数】parent：父容器
 * 【返回值】0=成功，-1=失败
 * =============================================================================
 */
int ui_floating_window_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return -1;
    }

    /* ---- 1. 创建悬浮窗主容器 ----
     * 使用 LV_OBJ_FLAG_FLOATING 让它脱离父布局，位置由 set_pos 控制 */
    g_float_win = lv_obj_create(parent);
    if (g_float_win == NULL) {
        return -1;
    }
    /* 悬浮窗尺寸：80x300，4 个 68x36 圆角按钮垂直排列 */
    lv_obj_set_size(g_float_win, 80, 300);
    /* 初始位置：屏幕右上角（1280 宽度 - 80 窗宽 - 20 边距 = 1180） */
    lv_obj_set_pos(g_float_win, 1180, 20);
    lv_obj_add_flag(g_float_win, LV_OBJ_FLAG_FLOATING);

    /* 启用 flex column 布局：4 个按钮垂直排列 */
    lv_obj_set_flex_flow(g_float_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_float_win, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(g_float_win, 6, 0);
    lv_obj_set_style_pad_row(g_float_win, 8, 0);

    /* 半透明深色背景，凸显悬浮感 */
    lv_obj_set_style_bg_opa(g_float_win, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(g_float_win, lv_color_hex(UI_COLOR_FLOAT_BG), 0);
    /* 圆角 16 */
    lv_obj_set_style_radius(g_float_win, 16, 0);
    /* 边框 1px 浅灰 */
    lv_obj_set_style_border_width(g_float_win, 1, 0);
    lv_obj_set_style_border_color(g_float_win, lv_color_hex(UI_COLOR_BORDER), 0);
    /* 轻微阴影 */
    lv_obj_set_style_shadow_width(g_float_win, 16, 0);
    lv_obj_set_style_shadow_opa(g_float_win, LV_OPA_20, 0);

    /* 注册拖动事件：按下并移动时触发拖动 */
    lv_obj_add_event_cb(g_float_win, float_win_drag_cb, LV_EVENT_PRESSING, NULL);

    /* ---- 2. 创建 4 个圆形按钮 ---- */

    /* 按钮 1：隐藏/显示主窗口 */
    lv_obj_t *btn_hide = lv_btn_create(g_float_win);
    lv_obj_set_size(btn_hide, 68, 36);
    lv_obj_set_style_radius(btn_hide, 8, 0);
    lv_obj_set_style_bg_color(btn_hide, lv_color_hex(0x55AA55), 0);
    lv_obj_t *lbl_hide = lv_label_create(btn_hide);
    lv_label_set_text(lbl_hide, "Hide");
    lv_obj_center(lbl_hide);
    lv_obj_add_event_cb(btn_hide, btn_hide_cb, LV_EVENT_CLICKED, NULL);

    /* 按钮 2：录音 */
    lv_obj_t *btn_mic = lv_btn_create(g_float_win);
    lv_obj_set_size(btn_mic, 68, 36);
    lv_obj_set_style_radius(btn_mic, 8, 0);
    lv_obj_set_style_bg_color(btn_mic, lv_color_hex(0x5588FF), 0);
    lv_obj_t *lbl_mic = lv_label_create(btn_mic);
    lv_label_set_text(lbl_mic, "Rec");
    lv_obj_center(lbl_mic);
    lv_obj_add_event_cb(btn_mic, btn_mic_cb, LV_EVENT_CLICKED, NULL);

    /* 按钮 3：截图 OCR */
    lv_obj_t *btn_ocr = lv_btn_create(g_float_win);
    lv_obj_set_size(btn_ocr, 68, 36);
    lv_obj_set_style_radius(btn_ocr, 8, 0);
    lv_obj_set_style_bg_color(btn_ocr, lv_color_hex(0xFFAA00), 0);
    lv_obj_t *lbl_ocr = lv_label_create(btn_ocr);
    lv_label_set_text(lbl_ocr, "OCR");
    lv_obj_center(lbl_ocr);
    lv_obj_add_event_cb(btn_ocr, btn_ocr_cb, LV_EVENT_CLICKED, NULL);

    /* 按钮 4：退出 */
    lv_obj_t *btn_exit = lv_btn_create(g_float_win);
    lv_obj_set_size(btn_exit, 68, 36);
    lv_obj_set_style_radius(btn_exit, 8, 0);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0xFF5555), 0);
    lv_obj_t *lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, "Exit");
    lv_obj_center(lbl_exit);
    lv_obj_add_event_cb(btn_exit, btn_exit_cb, LV_EVENT_CLICKED, NULL);

    printf("[ui_floating_window] 创建完成\n");
    return 0;
}
