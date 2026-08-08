/*
 * =============================================================================
 * EdgeSim UI 文件导入面板实现  ui_file_import.c
 * =============================================================================
 * 【文件作用】
 *   创建文件导入弹窗，包含：
 *     1. 文件路径输入框（用户手动填路径）
 *     2. 文件类型选择下拉框（TXT/MD/PDF/图片/音频）
 *     3. 导入按钮（按类型分发到对应业务子进程）
 *     4. 取消按钮（关闭面板）
 *   支持设计文档 1.1.2 节「本地私有 RAG 知识库」与 1.1.3 节「OCR 文字识别」。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.5 节「ui_lvgl（界面）」
 *
 * 【LVGL 控件树】
 *   lv_obj_t *panel (弹窗容器, 居中显示)
 *   ├── lv_obj_t *title      (标题: "Import File")
 *   ├── lv_obj_t *row_path   (路径行)
 *   │   ├── lv_obj_t *lbl_path   ("Path:")
 *   │   └── lv_obj_t *ta_path    (输入框)
 *   ├── lv_obj_t *row_type  (类型行)
 *   │   ├── lv_obj_t *lbl_type   ("Type:")
 *   │   └── lv_obj_t *dd_type    (下拉框)
 *   └── lv_obj_t *row_btn   (按钮行)
 *       ├── lv_obj_t *btn_import  ("导入")
 *       └── lv_obj_t *btn_cancel  ("Cancel")
 * =============================================================================
 */

#include "ui_lvgl.h"
#include "ui_pipeline.h"
#include "multi_proc.h"
#include "sqlite_db.h"     /* 阶段 4：kb_add 知识库文件入库 */
#include <stdio.h>
#include <string.h>

/* ---- 模块内部状态 ---- */
static lv_obj_t *g_panel     = NULL;   /* 弹窗主容器 */
static lv_obj_t *g_ta_path   = NULL;   /* 路径输入框 */
static lv_obj_t *g_dd_type   = NULL;   /* 类型下拉框 */

/* 文件类型枚举（与下拉框选项顺序对应） */
enum {
    FILE_TYPE_TXT = 0,   /* 文本：导入 RAG 知识库 */
    FILE_TYPE_MD,        /* Markdown：导入 RAG */
    FILE_TYPE_PDF,       /* PDF：导入 RAG（业务层负责解析） */
    FILE_TYPE_IMG,       /* 图片：发给 OCR 子进程 */
    FILE_TYPE_WAV,       /* 音频：发给 ASR 子进程 */
    FILE_TYPE_MAX
};


/*
 * -----------------------------------------------------------------------------
 * close_panel —— 内部辅助：隐藏面板（不销毁，下次复用）
 * -----------------------------------------------------------------------------
 */
static void close_panel(void)
{
    if (g_panel) {
        /* LV_OBJ_FLAG_HIDDEN 是 LVGL 隐藏标志，添加后控件不可见不响应事件 */
        lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    }
}


/*
 * -----------------------------------------------------------------------------
 * import_btn_event_cb —— 导入按钮点击回调
 * 【参数】e：LVGL 事件对象
 * 【说明】根据下拉框选择的类型，把文件路径发给对应子进程：
 *         TXT/MD/PDF → UI 进程处理（这里仅打印，真实项目转给 RAG 模块）
 *         IMG        → OCR 子进程
 *         WAV        → ASR 子进程
 * -----------------------------------------------------------------------------
 */
static void import_btn_event_cb(lv_event_t *e)
{
    (void)e;

    const char *path;
    uint16_t    sel;       /* 下拉框选中项索引 */

    /* 获取路径输入框文本 */
    path = lv_textarea_get_text(g_ta_path);
    if (path == NULL || path[0] == '\0') {
        printf("[ui_file_import] 路径为空\n");
        return;
    }

    /* lv_dropdown_get_selected 返回选中项索引，0 开始
     * LV_DROPDOWN_POS_LAST 表示未选择 */
    sel = lv_dropdown_get_selected(g_dd_type);

    switch (sel) {
    case FILE_TYPE_TXT:
    case FILE_TYPE_MD:
    case FILE_TYPE_PDF:
        /* 阶段 4：文本类文件入库 kb_files 表（RAG 检索的数据源）。
         * sel 与 kb_files 表 type 约定一致：TXT=0 / MD=1 / PDF=2。
         * 入库后，用户提问时 rag_retrieve 会检索该文件内容。
         * 注意：PDF(type=2) 虽入库，但 rag_retrieve 暂不解析 PDF
         * 二进制内容（检索时自动跳过），仅登记索引供后续扩展。 */
        if (kb_add(path, sel) < 0) {
            printf("[ui_file_import] 知识库入库失败: %s\n", path);
        } else {
            printf("[ui_file_import] 已导入知识库: %s (type=%u)\n", path, sel);
        }
        break;

    case FILE_TYPE_IMG:
        /* 图片：发给 OCR 子进程做识别
         * OCR 子进程收到 TASK_CMD_INFER 后会读取该图片并返回文字
         * 发送前先记录图片路径，供 on_ocr_result_cb 回调存入 kb_files 表 */
        ui_chat_window_set_ocr_image_path(path);
        if (ui_pipeline_send_infer(PROC_ID_OCR, path) != 0) {
            printf("[ui_file_import] 发送 OCR 任务失败\n");
            /* 发送失败要清空已记录的路径，避免下次 OCR 误用 */
            ui_chat_window_set_ocr_image_path(NULL);
        } else {
            printf("[ui_file_import] 已发送图片到 OCR: %s\n", path);
        }
        break;

    case FILE_TYPE_WAV:
        /* 音频：发给 ASR 子进程做转写 */
        if (ui_pipeline_send_infer(PROC_ID_ASR, path) != 0) {
            printf("[ui_file_import] 发送 ASR 任务失败\n");
        } else {
            printf("[ui_file_import] 已发送音频到 ASR: %s\n", path);
        }
        break;

    default:
        printf("[ui_file_import] 未知文件类型: %u\n", sel);
        break;
    }

    /* 关闭面板 */
    close_panel();
}


/*
 * -----------------------------------------------------------------------------
 * cancel_btn_event_cb —— 取消按钮点击回调
 * -----------------------------------------------------------------------------
 */
static void cancel_btn_event_cb(lv_event_t *e)
{
    (void)e;
    close_panel();
}


/*
 * =============================================================================
 * 公共接口：输入分发（供 ui_main 的 SDL 键盘事件调用）
 * =============================================================================
 * 【背景】ui_main.c 的 SDL_KEYDOWN/TEXTINPUT 事件原本直接发给
 *         ui_chat_window_handle_char，文件导入面板的输入框收不到输入。
 *         这里提供三个接口，让 ui_main 根据面板可见性分发键盘/粘贴事件。
 * ============================================================================= */

/*
 * ui_file_import_is_visible —— 查询文件导入面板当前是否可见
 * 【返回值】1=可见（应接收键盘输入），0=不可见（输入应发给聊天窗口）
 */
int ui_file_import_is_visible(void)
{
    if (g_panel == NULL) {
        return 0;
    }
    /* LV_OBJ_FLAG_HIDDEN 存在表示隐藏 */
    return (lv_obj_has_flag(g_panel, LV_OBJ_FLAG_HIDDEN) == false) ? 1 : 0;
}

/*
 * ui_file_import_handle_char —— 向路径输入框注入一个按键
 * 【参数】c：Unicode 码点（普通字符）或 LV_KEY_* 常量（特殊键）
 * 【说明】与 ui_chat_window_handle_char 对称，处理逻辑一致：
 *         回车=触发导入，退格=删除，方向键=移动光标，普通字符=插入。
 */
void ui_file_import_handle_char(uint32_t c)
{
    if (g_ta_path == NULL) {
        return;
    }

    switch (c) {
    case LV_KEY_BACKSPACE:
        lv_textarea_del_char(g_ta_path);
        break;
    case LV_KEY_DEL:
        lv_textarea_del_char_forward(g_ta_path);
        break;
    case LV_KEY_LEFT:
        lv_textarea_cursor_left(g_ta_path);
        break;
    case LV_KEY_RIGHT:
        lv_textarea_cursor_right(g_ta_path);
        break;
    case LV_KEY_ENTER:
        /* one_line 模式下回车触发导入（与聊天窗口回车=发送一致） */
        import_btn_event_cb(NULL);
        break;
    default:
        /* 可打印字符（>=32）插入到光标位置 */
        if (c >= 32) {
            /* 【阶段 4.5 修复：与聊天窗同策略】
             * lv_textarea_add_char 对中文码点会拆成字节导致乱码，
             * 改用手动 UTF-8 编码 + lv_textarea_add_text（与粘贴同路径）。
             * 详见 ui_chat_window.c 同名修复注释。 */
            char utf8_buf[8];       /* 单字符 UTF-8 编码缓冲 */
            int  n = 0;             /* 已编码字节数 */
            if (c < 0x80) {
                utf8_buf[n++] = (char)c;
            } else if (c < 0x800) {
                utf8_buf[n++] = (char)(0xC0 | (c >> 6));
                utf8_buf[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                utf8_buf[n++] = (char)(0xE0 | (c >> 12));
                utf8_buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8_buf[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                utf8_buf[n++] = (char)(0xF0 | (c >> 18));
                utf8_buf[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                utf8_buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8_buf[n++] = (char)(0x80 | (c & 0x3F));
            }
            utf8_buf[n] = '\0';
            lv_textarea_add_text(g_ta_path, utf8_buf);
        }
        break;
    }
}

/*
 * ui_file_import_paste —— 粘贴文本到路径输入框
 * 【参数】text：要粘贴的文本（UTF-8）
 */
void ui_file_import_paste(const char *text)
{
    if (g_ta_path != NULL && text != NULL) {
        lv_textarea_add_text(g_ta_path, text);
    }
}


/*
 * =============================================================================
 * 公共接口：ui_file_import_create / ui_file_import_show
 * =============================================================================
 * 【参数】parent：父容器
 * 【返回值】0=成功，-1=失败
 * 【说明】创建面板并初始隐藏。需要时调用 ui_file_import_show() 显示。
 * =============================================================================
 */
int ui_file_import_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return -1;
    }

    /* ---- 1. 创建弹窗容器（居中，固定大小） ----
     * 初始隐藏，由 ui_file_import_show 显示 */
    g_panel = lv_obj_create(parent);
    if (g_panel == NULL) {
        return -1;
    }
    /* 弹窗尺寸 480x360（适配 1280x720 桌面窗口） */
    lv_obj_set_size(g_panel, 480, 360);
    /* 居中显示：lv_obj_center 让控件在父容器中居中 */
    lv_obj_center(g_panel);
    /* 初始隐藏 */
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    /* 启用 flex column 布局 */
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_panel, 16, 0);
    lv_obj_set_style_pad_row(g_panel, 12, 0);
    /* 弹窗浮在最上层：LV_OBJ_FLAG_FLOATING 让控件脱离父布局影响 */
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_FLOATING);
    /* 弹窗样式：主背景色 + 圆角 12 + 阴影 */
    lv_obj_set_style_bg_color(g_panel, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_radius(g_panel, 12, 0);
    lv_obj_set_style_border_width(g_panel, 1, 0);
    lv_obj_set_style_border_color(g_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(g_panel, 32, 0);
    lv_obj_set_style_shadow_opa(g_panel, LV_OPA_20, 0);

    /* ---- 2. 标题 ---- */
    lv_obj_t *title = lv_label_create(g_panel);
    lv_label_set_text(title, "Import File");
    lv_obj_set_style_text_font(title, UI_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);

    /* ---- 3. 路径行（水平 flex） ---- */
    lv_obj_t *row_path = lv_obj_create(g_panel);
    lv_obj_set_size(row_path, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row_path, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row_path, 0, 0);
    lv_obj_set_style_border_width(row_path, 0, 0);
    lv_obj_set_flex_align(row_path, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_path = lv_label_create(row_path);
    lv_label_set_text(lbl_path, "Path:");
    /* 固定宽度让冒号对齐 */
    lv_obj_set_width(lbl_path, 50);

    g_ta_path = lv_textarea_create(row_path);
    lv_obj_set_flex_grow(g_ta_path, 1);
    lv_textarea_set_placeholder_text(g_ta_path, "/path/to/file");
    lv_textarea_set_one_line(g_ta_path, true);

    /* ---- 4. 类型行 ---- */
    lv_obj_t *row_type = lv_obj_create(g_panel);
    lv_obj_set_size(row_type, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row_type, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row_type, 0, 0);
    lv_obj_set_style_border_width(row_type, 0, 0);
    lv_obj_set_flex_align(row_type, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_type = lv_label_create(row_type);
    lv_label_set_text(lbl_type, "Type:");
    lv_obj_set_width(lbl_type, 50);

    /* 下拉框：选项用 \n 分隔的字符串
     * lv_dropdown_create 默认显示第一个选项 */
    g_dd_type = lv_dropdown_create(row_type);
    lv_dropdown_set_options(g_dd_type,
                             "TXT\n"
                             "Markdown\n"
                             "PDF\n"
                             "Image (OCR)\n"
                             "Audio (ASR)");
    lv_obj_set_flex_grow(g_dd_type, 1);

    /* ---- 5. 按钮行 ---- */
    lv_obj_t *row_btn = lv_obj_create(g_panel);
    lv_obj_set_size(row_btn, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btn, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row_btn, 0, 0);
    lv_obj_set_style_border_width(row_btn, 0, 0);
    /* 按钮之间间距 8px */
    lv_obj_set_style_pad_column(row_btn, 8, 0);

    lv_obj_t *btn_import = lv_btn_create(row_btn);
    lv_obj_t *lbl_import = lv_label_create(btn_import);
    lv_label_set_text(lbl_import, "Import");
    lv_obj_add_event_cb(btn_import, import_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_cancel = lv_btn_create(row_btn);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_add_event_cb(btn_cancel, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);

    printf("[ui_file_import] 创建完成\n");
    return 0;
}


/*
 * ui_file_import_show —— 显示文件导入面板
 * 【返回值】0=成功，-1=失败
 */
int ui_file_import_show(void)
{
    if (g_panel == NULL) {
        return -1;
    }
    /* 移除隐藏标志，使控件可见 */
    lv_obj_clear_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    /* 强制重新布局并刷新显示 */
    lv_obj_update_layout(g_panel);
    return 0;
}
