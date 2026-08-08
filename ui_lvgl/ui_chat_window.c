/*
 * =============================================================================
 * EdgeSim UI 主对话窗口实现  ui_chat_window.c
 * =============================================================================
 * 【文件作用】
 *   创建主对话窗口，包含：
 *     1. 消息列表（滚动显示用户与 AI 的对话历史）
 *     2. 输入框（用户输入问题）
 *     3. 发送按钮（点击后通过管道发给 LLM 子进程）
 *   本文件仅做界面与事件处理，AI 推理由业务层完成。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.5 节「ui_lvgl（界面）」
 *
 * 【LVGL 控件树（核心）】
 *   lv_obj_t *chat_cont (容器, flex column)
 *   ├── lv_obj_t *msg_list  (滚动容器, 显示消息)
 *   │   ├── lv_obj_t *msg_user_1    (用户气泡)
 *   │   ├── lv_obj_t *msg_ai_1      (AI 气泡)
 *   │   └── ...
 *   └── lv_obj_t *input_row (容器, flex row)
 *       ├── lv_obj_t *ta_input    (文本域)
 *       └── lv_obj_t *btn_send    (发送按钮)
 * =============================================================================
 */

#include "ui_lvgl.h"
#include "ui_pipeline.h"
#include "multi_proc.h"
#include "sqlite_db.h"          /* chat_save / kb_add 持久化 OCR 结果 */
#include "rag.h"                /* 阶段 4：rag_retrieve 本地知识库检索 */
#include <stdio.h>
#include <string.h>

/* ---- 模块内部状态 ---- */
static lv_obj_t *g_msg_list = NULL;     /* 消息列表容器（滚动） */
static lv_obj_t *g_ta_input  = NULL;    /* 输入文本域 */

/* 最近一次 OCR 识别对应的图片路径
 * ui_file_import 发送 OCR 任务前调用 ui_chat_window_set_ocr_image_path 记录，
 * on_ocr_result_cb 触发时读取此路径存入 kb_files 表
 * KB_PATH_MAX 在 sqlite_db.h 中定义为 256 */
static char g_last_ocr_image_path[KB_PATH_MAX] = {0};

/* ---- 阶段 4.5：RAG 来源标注状态 ----
 * send_btn_event_cb 发送问题时，若 rag_retrieve 命中知识库，
 * 用 parse_rag_sources 把命中的文件路径记录到 g_last_rag_sources；
 * on_llm_reply_cb 展示 LLM 回复时，在气泡末尾附加"[来源] 文件名"标注，
 * 便于用户追溯答案来自哪个知识库（多知识库合并检索时逐一列出）。
 * 每次发送新问题都会刷新该记录；未命中知识库时计数清为 0。 */
#define RAG_SOURCE_MAX  4          /* 最多记录 4 个来源（与 rag Top N 一致） */
static char g_last_rag_sources[RAG_SOURCE_MAX][KB_PATH_MAX]; /* 来源路径列表 */
static int   g_last_rag_source_cnt = 0;                       /* 来源个数 */

/* 外部函数声明：ui_file_import 提供显示面板接口
 * ui_chat_window 工具栏的"Import"按钮调用它弹出文件导入弹窗 */
extern int ui_file_import_show(void);

/* 外部函数声明：ui_kb_manager 提供知识库管理面板接口（阶段 6）
 * 工具栏"KB"按钮调用它弹出知识库列表（查看/搜索/排序） */
extern int ui_kb_manager_show(void);

/* 前向声明：ui_chat_window_handle_char 中回车键需要调用发送回调 */
static void send_btn_event_cb(lv_event_t *e);

/* 每条消息气泡最大宽度（占消息列表的百分比）
 * LVGL flex 布局下用百分比，0~10000 表示 0~100% */
#define MSG_BUBBLE_MAX_WIDTH_PCT  8000   /* 80% */

/* 用户输入最大长度（与 TASK_DATA_MAX 保持一致） */
#define CHAT_INPUT_MAX_LEN  4096

/* 消息列表保留的最大气泡数（阶段 4.5 性能优化）
 * 背景：LVGL 8.2 无字形缓存，聊天记录越多，每次重绘渲染的
 *       中文字形越多，界面越卡（用户实测"加载内容增多后卡顿"）。
 * 方案：超过上限删除最老气泡，控制每帧渲染对象数量。
 *       历史消息已由 chat_save 持久化到数据库，UI 仅保留最近 N 条，
 *       不影响数据完整性（重启后从数据库可查）。 */
#define CHAT_MAX_BUBBLES  100


/*
 * -----------------------------------------------------------------------------
 * bubble_click_cb —— 消息气泡点击回调（点击复制内容到剪贴板）
 * ----------------------------------------------------------------------------- */
static void bubble_click_cb(lv_event_t *e)
{
    /* lv_event_get_current_target 返回事件回调注册的对象（bubble），
     * 而 lv_event_get_target 可能返回子对象（label） */
    lv_obj_t *bubble = lv_event_get_current_target(e);
    /* 获取气泡内的 label 对象 */
    lv_obj_t *label = lv_obj_get_child(bubble, 0);
    if (label != NULL) {
        const char *text = lv_label_get_text(label);
        if (text != NULL) {
            ui_clipboard_copy(text);
        }
    }
}


/*
 * -----------------------------------------------------------------------------
 * add_message_bubble —— 内部辅助：在消息列表添加一条气泡
 * 【参数】text    ：消息文本
 * 【参数】is_user ：1=用户气泡(右对齐), 0=AI气泡(左对齐)
 * 【返回值】指向气泡对象的指针，失败返回 NULL
 * -----------------------------------------------------------------------------
 */
static lv_obj_t *add_message_bubble(const char *text, int is_user)
{
    /* 创建容器作为气泡本体，使用 flex 布局 */
    lv_obj_t *bubble = lv_obj_create(g_msg_list);
    if (bubble == NULL) {
        return NULL;
    }

    /* 设置气泡尺寸：宽度占 80%，高度随内容自适应
     * LV_SIZE_CONTENT 用于子控件自身高度是安全的（不会触发父容器重排） */
    lv_obj_set_width(bubble, lv_pct(MSG_BUBBLE_MAX_WIDTH_PCT / 100));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    /* 内边距 10px，文字不贴边 */
    lv_obj_set_style_pad_all(bubble, 10, 0);

    /* 移除默认边框，改为背景色区分用户/AI */
    lv_obj_set_style_border_width(bubble, 0, 0);
    /* 圆角 12px（现代 app 风格） */
    lv_obj_set_style_radius(bubble, 12, 0);

    if (is_user) {
        /* 用户气泡：蓝色背景 + 白色文字 + 右对齐 */
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_COLOR_USER_BUBBLE), 0);
        lv_obj_set_style_text_color(bubble, lv_color_hex(UI_COLOR_TEXT_WHITE), 0);
        /* 右对齐：设置 bubble 自身的 cross-axis 对齐为 END
         * 注意：必须用 lv_obj_set_style_align 作用于 bubble 自身，
         * 绝不能用 lv_obj_set_style_flex_main_place 修改父容器 g_msg_list，
         * 否则会触发父容器整体布局重算，导致 lv_timer_handler 重入死循环 */
        lv_obj_set_style_align(bubble, LV_FLEX_ALIGN_END, 0);
    } else {
        /* AI 气泡：浅灰背景 + 深色文字 + 左对齐 */
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_COLOR_AI_BUBBLE), 0);
        lv_obj_set_style_text_color(bubble, lv_color_hex(UI_COLOR_TEXT), 0);
        /* 左对齐：cross-axis 对齐为 START */
        lv_obj_set_style_align(bubble, LV_FLEX_ALIGN_START, 0);
    }

    /* 在气泡内创建 Label 显示文字 */
    lv_obj_t *label = lv_label_create(bubble);
    if (label == NULL) {
        lv_obj_del(bubble);
        return NULL;
    }
    /* lv_label_set_text 内部会 strdup，无需调用者保留 text 内存 */
    lv_label_set_text(label, text);
    /* 中文支持（阶段 4.5）：气泡文本统一用中文字体。
     * UI_FONT_CHINESE 在 HAS_CHINESE_FONT 宏下指向 lv_font_chinese_16
     * （含 ASCII + GB2312 常用汉字）；未生成中文字体时回退 montserrat_14
     * （仅英文），不影响原有英文显示。 */
    lv_obj_set_style_text_font(label, UI_FONT_CHINESE, 0);
    /* 长文本模式：自动换行 */
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    /* 宽度限制为气泡内部宽度（减去内边距） */
    lv_obj_set_width(label, lv_pct(100));

    /* 启用点击复制：点击气泡将文本复制到系统剪贴板 */
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bubble, bubble_click_cb, LV_EVENT_CLICKED, NULL);

    /* 【阶段 4.5 性能优化：限制消息列表最大条数】
     * 超出 CHAT_MAX_BUBBLES 时删除最老的气泡，控制渲染对象数量，
     * 避免聊天记录无限增长导致中文字形重绘开销越来越大（卡顿）。
     * 注意：
     *   - 只删除"最老"气泡，不影响本次新建的 bubble（调用方仍可用）
     *   - 历史消息已持久化到数据库（chat_save），UI 删气泡无数据损失
     *   - lv_obj_get_child(g_msg_list, 0) 是列表中最先添加的子对象 */
    while (lv_obj_get_child_cnt(g_msg_list) > CHAT_MAX_BUBBLES) {
        lv_obj_t *oldest = lv_obj_get_child(g_msg_list, 0);
        if (oldest == NULL) {
            break;   /* 防御：理论上不会走到（有气泡才进循环） */
        }
        lv_obj_del(oldest);
    }

    return bubble;
}


/*
 * -----------------------------------------------------------------------------
 * import_btn_cb —— 工具栏"Import"按钮回调
 * 【说明】调用 ui_file_import_show 弹出文件导入面板
 *         用户可在面板中选择文件并导入到 RAG 知识库 / OCR / ASR
 * -----------------------------------------------------------------------------
 */
static void import_btn_cb(lv_event_t *e)
{
    (void)e;
    if (ui_file_import_show() != 0) {
        printf("[ui_chat_window] 文件导入面板显示失败\n");
    }
}


/*
 * -----------------------------------------------------------------------------
 * kb_btn_cb —— 工具栏"KB"按钮回调（阶段 6）
 * 【说明】调用 ui_kb_manager_show 弹出知识库管理面板
 *         用户可查看已导入的知识库列表（名称/文件数/时间，支持搜索排序）
 * -----------------------------------------------------------------------------
 */
static void kb_btn_cb(lv_event_t *e)
{
    (void)e;
    if (ui_kb_manager_show() != 0) {
        printf("[ui_chat_window] 知识库管理面板显示失败\n");
    }
}


/*
 * -----------------------------------------------------------------------------
 * clear_btn_cb —— 工具栏"Clear"按钮回调
 * 【说明】清空消息列表所有气泡，重新开始对话
 *         通过 lv_obj_clean 删除容器所有子控件
 * -----------------------------------------------------------------------------
 */
static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_msg_list != NULL) {
        /* lv_obj_clean 删除容器的所有子控件（不删除容器本身） */
        lv_obj_clean(g_msg_list);
        /* 添加一条系统提示 */
        add_message_bubble("Conversation cleared.", 0);
    }
}


/*
 * -----------------------------------------------------------------------------
 * ui_chat_window_scroll —— 滚动消息列表（供 ui_main 的滚轮事件调用）
 * 【参数】dy：滚动像素数（正值=向下滚动，负值=向上滚动）
 * ----------------------------------------------------------------------------- */
void ui_chat_window_scroll(int32_t dy)
{
    if (g_msg_list != NULL) {
        /* lv_obj_scroll_by 对对象内容进行滚动
         * LV_ANIM_ON 开启滚动动画 */
        lv_obj_scroll_by(g_msg_list, 0, dy, LV_ANIM_ON);
    }
}


/*
 * -----------------------------------------------------------------------------
 * ui_chat_window_paste —— 粘贴文本到输入框（供 Ctrl+V 调用）
 * ----------------------------------------------------------------------------- */
void ui_chat_window_paste(const char *text)
{
    if (g_ta_input != NULL && text != NULL) {
        /* lv_textarea_add_text 在光标位置插入文本 */
        lv_textarea_add_text(g_ta_input, text);
    }
}


/*
 * -----------------------------------------------------------------------------
 * ui_chat_window_handle_char —— 直接处理一个按键（绕过 LVGL indev 30ms 周期）
 * 【参数】c：Unicode 码点（普通字符）或 LV_KEY_* 常量（特殊键）
 * 【说明】ui_main 的 SDL 事件直接调用此函数，不经过 LVGL indev 系统，
 *         避免连续输入时 indev 读取周期导致按键积压卡顿。
 * ----------------------------------------------------------------------------- */
void ui_chat_window_handle_char(uint32_t c)
{
    if (g_ta_input == NULL) return;

    switch (c) {
    case LV_KEY_BACKSPACE:
        lv_textarea_del_char(g_ta_input);
        break;
    case LV_KEY_DEL:
        lv_textarea_del_char_forward(g_ta_input);
        break;
    case LV_KEY_LEFT:
        lv_textarea_cursor_left(g_ta_input);
        break;
    case LV_KEY_RIGHT:
        lv_textarea_cursor_right(g_ta_input);
        break;
    case LV_KEY_UP:
        lv_textarea_cursor_up(g_ta_input);
        break;
    case LV_KEY_DOWN:
        lv_textarea_cursor_down(g_ta_input);
        break;
    case LV_KEY_HOME:
        lv_textarea_set_cursor_pos(g_ta_input, 0);
        break;
    case LV_KEY_END: {
        const char *txt = lv_textarea_get_text(g_ta_input);
        lv_textarea_set_cursor_pos(g_ta_input, txt ? (int)strlen(txt) : 0);
        break;
    }
    case LV_KEY_ENTER:
        send_btn_event_cb(NULL);
        break;
    default:
        if (c >= 32) {
            /* 【阶段 4.5 修复：中文输入乱码问题】
             * 现象：lv_textarea_add_char(g_ta_input, 0x4F60) 插入中文后
             *       显示为乱码（"你好" → "}YO"，码点被拆成高低字节），
             *       而粘贴（lv_textarea_add_text 整串 UTF-8）显示正常。
             * 修复：键盘输入改走与粘贴相同的 add_text 路径——
             *       先把 Unicode 码点手动编码为 UTF-8 字节串，
             *       再整串插入，绕开 add_char 对非 ASCII 码点的处理问题。
             * 【UTF-8 编码规则】
             *   <0x80    → 1 字节（ASCII）
             *   <0x800   → 2 字节
             *   <0x10000 → 3 字节（汉字，如 你=0x4F60 → E4 BD A0）
             *   其他     → 4 字节（emoji 等扩展字符） */
            char utf8_buf[8];       /* 单字符 UTF-8 编码缓冲（≤4 字节 + '\0'） */
            int  n = 0;             /* 已编码字节数 */
            if (c < 0x80) {
                utf8_buf[n++] = (char)c;                    /* 1 字节 ASCII */
            } else if (c < 0x800) {
                utf8_buf[n++] = (char)(0xC0 | (c >> 6));    /* 2 字节 */
                utf8_buf[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                utf8_buf[n++] = (char)(0xE0 | (c >> 12));   /* 3 字节（汉字） */
                utf8_buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8_buf[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                utf8_buf[n++] = (char)(0xF0 | (c >> 18));   /* 4 字节 */
                utf8_buf[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                utf8_buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8_buf[n++] = (char)(0x80 | (c & 0x3F));
            }
            utf8_buf[n] = '\0';
            /* 与粘贴同路径：整串 UTF-8 插入，确保中文正常显示 */
            lv_textarea_add_text(g_ta_input, utf8_buf);
        }
        break;
    }
}


/*
 * -----------------------------------------------------------------------------
 * parse_rag_sources —— 解析 RAG 上下文中命中的知识库来源（阶段 4.5 新增）
 * 【参数】rag_ctx：rag_retrieve 输出的拼接结果
 * 【格式】rag_ctx 由 rag_retrieve 拼成，每段为：
 *           [知识库: <文件路径>]
 *           <片段文本>
 *         本函数提取每个 "[知识库: " 后的路径到 g_last_rag_sources，
 *         供 LLM 回复气泡末尾标注来源。最多 RAG_SOURCE_MAX 个。
 * -----------------------------------------------------------------------------
 */
static void parse_rag_sources(const char *rag_ctx)
{
    const char *p;              /* 扫描指针 */

    g_last_rag_source_cnt = 0;
    if (rag_ctx == NULL || rag_ctx[0] == '\0') {
        return;                 /* 无命中，来源为空 */
    }

    p = rag_ctx;
    while (g_last_rag_source_cnt < RAG_SOURCE_MAX) {
        /* 1. 找下一个 "[知识库: " 标注起点 */
        p = strstr(p, "[知识库: ");
        if (p == NULL) {
            break;              /* 没有更多来源 */
        }
        p += strlen("[知识库: ");

        /* 2. 提取路径：到换行符或字符串结束为止 */
        {
            const char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= KB_PATH_MAX) {
                len = KB_PATH_MAX - 1;   /* 限长防越界 */
            }
            memcpy(g_last_rag_sources[g_last_rag_source_cnt], p, len);
            g_last_rag_sources[g_last_rag_source_cnt][len] = '\0';
            g_last_rag_source_cnt++;

            /* 3. 前进到下一段（跳过已消费的路径及其换行） */
            p = end ? end + 1 : p + len;
        }
    }
}


/*
 * -----------------------------------------------------------------------------
 * send_btn_event_cb —— 发送按钮事件回调
 * 【参数】e：LVGL 事件对象，包含事件类型与触发控件
 * 【说明】用户点击按钮或按回车时触发：
 *         1. 读取输入框文本
 *         2. 在消息列表追加用户气泡
 *         3. 清空输入框
 *         4. 通过管道发给 LLM 子进程（不阻塞 UI）
 *         5. AI 回复通过 ui_pipeline 回调异步追加
 * -----------------------------------------------------------------------------
 */
static void send_btn_event_cb(lv_event_t *e)
{
    (void)e;   /* 这里通过全局 g_ta_input 取值，不使用事件参数 */

    const char *text;

    /* lv_textarea_get_text 返回内部缓冲区指针，无需 free
     * 但内容会被后续 lv_textarea_set_text 覆盖，需先取出来用 */
    text = lv_textarea_get_text(g_ta_input);
    if (text == NULL || text[0] == '\0') {
        /* 空输入不发送 */
        return;
    }

    /* 在消息列表追加用户气泡（is_user=1） */
    add_message_bubble(text, 1);

    /* ---- 阶段 4：RAG 检索，拼接 LLM 上下文 ----
     * 从知识库（kb_files 中的 TXT/MD）检索与问题最相关的片段，
     * 拼接到提示词前作为"参考资料"，让 LLM 基于本地知识回答。
     * 无命中/知识库为空时直接用原始文本。
     * 注意：TaskData.data_buf 上限 TASK_DATA_MAX=4096，
     *       snprintf 会自动截断超长内容，保证不越界。 */
    char rag_ctx[RAG_CONTEXT_MAX];
    char full_prompt[TASK_DATA_MAX];
    if (rag_retrieve(text, rag_ctx, sizeof(rag_ctx), RAG_DEFAULT_TOP_N) == 0
        && rag_ctx[0] != '\0') {
        snprintf(full_prompt, sizeof(full_prompt),
                 "[参考资料]\n%s\n\n[用户问题]\n%s", rag_ctx, text);
        printf("[ui_chat_window] RAG 命中知识库，已拼接上下文 (%zu 字节)\n",
               strlen(rag_ctx));
        /* 阶段 4.5：记录命中的知识库来源，供回复气泡末尾标注 */
        parse_rag_sources(rag_ctx);
    } else {
        snprintf(full_prompt, sizeof(full_prompt), "%s", text);
        /* 未命中知识库：清空来源标注（回复不附加来源） */
        g_last_rag_source_cnt = 0;
    }

    /* 通过管道发给 LLM 子进程
     * PROC_ID_LLM 在 multi_proc.h 中定义为 1 */
    if (ui_pipeline_send_infer(PROC_ID_LLM, full_prompt) != 0) {
        /* 发送失败：在消息列表提示 */
        add_message_bubble("[System] Send failed, LLM not responding", 0);
    }

    /* 清空输入框，准备下次输入 */
    lv_textarea_set_text(g_ta_input, "");
}


/*
 * -----------------------------------------------------------------------------
 * on_llm_reply_cb —— LLM 回复到达时的回调（由 ui_pipeline 调用）
 * 【参数】text：LLM 回复文本
 * 【说明】直接在消息列表追加 AI 气泡。由于 ui_pipeline timer 在 LVGL
 *         线程中调用，本回调可直接操作 LVGL 控件，无需锁。
 * -----------------------------------------------------------------------------
 */
static void on_llm_reply_cb(const char *text)
{
    if (text == NULL) {
        return;
    }

    /* ---- 阶段 4.5：RAG 来源标注 ----
     * 若本次问题命中了知识库（send_btn_event_cb 已记录来源），
     * 在回复气泡末尾附加"[来源] 文件名"标注，便于用户追溯
     * 答案来自哪个知识库并评估可信度。
     * 多个知识库命中时逐一列出文件名（顿号分隔）。
     * 展示形式为气泡内脚注式小段落（跟随回复文本，非独立气泡）。 */
    if (g_last_rag_source_cnt > 0) {
        char buf[TASK_DATA_MAX + KB_PATH_MAX * RAG_SOURCE_MAX + 64];
        int  n;
        int  i;

        /* 1. 回复原文 + 空行 + "[来源] " 前缀 */
        n = snprintf(buf, sizeof(buf), "%s\n\n[来源] ", text);

        /* 2. 逐一列出来源文件名（取 basename，去掉目录前缀更清晰） */
        for (i = 0; i < g_last_rag_source_cnt; i++) {
            const char *path = g_last_rag_sources[i];
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            /* 多来源用顿号分隔；snprintf 自动防溢出截断 */
            n += snprintf(buf + n, sizeof(buf) - n, "%s%s",
                          i > 0 ? "、" : "", base);
        }
        add_message_bubble(buf, 0);
    } else {
        /* 未命中知识库：正常显示回复，不附加来源 */
        add_message_bubble(text, 0);
    }

    /* 滚动消息列表到最后一条（让用户看到最新回复）
     * lv_obj_scroll_to_y 参数：
     *   obj     ：滚动容器
     *   y       ：目标 y 位置（这里用 LV_COORD_MAX 表示最底部）
     *   anim    ：动画开关（LV_ANIM_ON=平滑滚动） */
    lv_obj_scroll_to_y(g_msg_list, LV_COORD_MAX, LV_ANIM_ON);
}


/*
 * -----------------------------------------------------------------------------
 * on_ocr_result_cb —— OCR 识别结果到达时的回调（由 ui_pipeline 调用）
 * 【参数】text：OCR 识别出的文字
 * 【说明】OCR 结果作为 AI 消息显示（左对齐气泡），加 [OCR] 前缀
 *         标识是图片文字识别结果。使用中文字体显示中文内容。
 *         同时把识别结果与图片路径持久化到 SQLite 数据库：
 *           - 识别文本 → chat_history 表（role=1 AI 消息）
 *           - 图片路径 → kb_files 表（type=3 图片）
 *         识别为空/失败时，图片路径仍入库，chat_history 存错误标记，便于追溯。
 * -----------------------------------------------------------------------------
 */
static void on_ocr_result_cb(const char *text)
{
    char buf[TASK_DATA_MAX + 16];
    lv_obj_t *bubble;
    lv_obj_t *label;
    int recognize_ok = 1;       /* 1=识别成功(有文本), 0=识别失败/空 */
    int chat_id;                /* chat_history 新记录 ID */
    int kb_id;                  /* kb_files 新记录 ID */

    if (text == NULL) {
        return;
    }

    /* 判断识别是否成功：
     * - text 为空字符串 → 失败
     * - text 以 "[System]" 开头 → 子进程返回的失败标记（如 Inference failed）
     *   （main.c 中 output 为空或 run 失败时统一发 "[System] Inference failed..."）*/
    if (text[0] == '\0' || strncmp(text, "[System]", 8) == 0) {
        recognize_ok = 0;
    }

    /* ---- 1. 显示气泡 ----
     * 识别成功：显示 "[OCR] 识别文本"
     * 识别失败：显示 "[OCR] 识别失败" 提示用户 */
    if (recognize_ok) {
        snprintf(buf, sizeof(buf), "[OCR] %s", text);
    } else {
        snprintf(buf, sizeof(buf), "[OCR] 识别失败或图片无文字");
    }
    bubble = add_message_bubble(buf, 0);

    /* 为 OCR 结果气泡设置中文字体（识别结果可能含中文） */
    if (bubble != NULL) {
        lv_obj_set_style_text_font(bubble, UI_FONT_CHINESE, 0);
        label = lv_obj_get_child(bubble, 0);
        if (label != NULL) {
            lv_obj_set_style_text_font(label, UI_FONT_CHINESE, 0);
        }
    }

    lv_obj_scroll_to_y(g_msg_list, LV_COORD_MAX, LV_ANIM_ON);

    /* ---- 2. 持久化到数据库 ----
     * chat_save：存识别文本到 chat_history 表（role=1 表示 AI 消息）
     *   - 识别成功：存 "[OCR] 识别文本"
     *   - 识别失败：存 "[OCR] 识别失败" 便于历史追溯
     * kb_add：存图片路径到 kb_files 表（type=3 表示 image）
     *   - 无论识别成功与否都存路径，便于后续重试或人工查看
     * 返回值 >0 表示新记录 ID，-1 表示失败（仅打日志，不影响 UI 显示）*/
    chat_id = chat_save(NULL, 1, buf);
    if (chat_id < 0) {
        printf("[ui_chat_window] chat_save 失败\n");
    }

    if (g_last_ocr_image_path[0] != '\0') {
        kb_id = kb_add(g_last_ocr_image_path, 3);
        if (kb_id < 0) {
            printf("[ui_chat_window] kb_add 失败: %s\n", g_last_ocr_image_path);
        } else {
            printf("[ui_chat_window] 图片路径已入库 kb_files (id=%d): %s\n",
                   kb_id, g_last_ocr_image_path);
        }
        /* 用完即清空，避免下次 OCR 复用旧路径 */
        g_last_ocr_image_path[0] = '\0';
    }

    printf("[ui_chat_window] OCR 结果已显示%s: %s\n",
           recognize_ok ? "" : "(失败)", text);
}


/*
 * -----------------------------------------------------------------------------
 * ui_chat_window_set_ocr_image_path —— 记录最近一次 OCR 识别的图片路径
 * 【参数】path：图片文件路径（NULL 或空串表示清空记录）
 * 【说明】ui_file_import 在发送 OCR 任务前调用此函数。
 *         由于 ui_pipeline 的 OCR 回调签名只传识别文本不传原始图片路径，
 *         需先用此函数把路径暂存到模块静态变量，on_ocr_result_cb 触发时再读取。
 *         路径长度超过 KB_PATH_MAX-1 会被截断。
 * -----------------------------------------------------------------------------
 */
void ui_chat_window_set_ocr_image_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        /* 清空记录 */
        g_last_ocr_image_path[0] = '\0';
        return;
    }

    /* strncpy 不会自动补 '\0'，手动确保截断后以 '\0' 结尾 */
    strncpy(g_last_ocr_image_path, path, KB_PATH_MAX - 1);
    g_last_ocr_image_path[KB_PATH_MAX - 1] = '\0';
}


/*
 * -----------------------------------------------------------------------------
 * on_asr_result_cb —— ASR 转写结果到达时的回调（由 ui_pipeline 调用）
 * 【参数】text：ASR 转写出的文字
 * 【说明】语音转写结果作为用户消息显示（右对齐气泡），加 [Voice] 前缀
 *         区分是语音输入还是键盘输入。后续可自动发给 LLM 进行对话。
 * -----------------------------------------------------------------------------
 */
static void on_asr_result_cb(const char *text)
{
    char buf[TASK_DATA_MAX + 16];
    lv_obj_t *bubble;
    lv_obj_t *label;

    if (text == NULL) {
        return;
    }

    /* 加 [Voice] 前缀标识语音输入 */
    snprintf(buf, sizeof(buf), "[Voice] %s", text);
    bubble = add_message_bubble(buf, 1);

    /* 为 ASR 结果气泡单独设置中文字体（不影响其他 UI 元素）
     * 设置 bubble 和 label 的字体为 UI_FONT_CHINESE，支持中文显示 */
    if (bubble != NULL) {
        lv_obj_set_style_text_font(bubble, UI_FONT_CHINESE, 0);
        label = lv_obj_get_child(bubble, 0);
        if (label != NULL) {
            lv_obj_set_style_text_font(label, UI_FONT_CHINESE, 0);
        }
    }

    /* 滚动到最新消息 */
    lv_obj_scroll_to_y(g_msg_list, LV_COORD_MAX, LV_ANIM_ON);

    printf("[ui_chat_window] ASR 结果已显示: %s\n", text);
}


/*
 * =============================================================================
 * 公共接口：ui_chat_window_create
 * =============================================================================
 * 【参数】parent：父容器（一般是 lv_scr_act() 当前屏幕）
 * 【返回值】0=成功，-1=失败
 * 【说明】在 parent 上创建整个对话窗口布局，注册 LLM 回复回调。
 *         在 ui_init 中调用一次。
 * =============================================================================
 */
int ui_chat_window_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return -1;
    }

    /* ---- 1. 创建主容器（垂直 flex 布局，占满父容器） ---- */
    lv_obj_t *cont = lv_obj_create(parent);
    if (cont == NULL) {
        return -1;
    }
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    /* 内边距 0：工具栏/消息列表/输入区各自管理自己的内边距 */
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(UI_COLOR_BG), 0);

    /* ---- 2. 创建顶部工具栏（标题 + 导入文件 + 清空对话） ----
     * 固定高度 56px，水平 flex，次背景色 + 底部边框分隔 */
    lv_obj_t *toolbar = lv_obj_create(cont);
    if (toolbar == NULL) {
        return -1;
    }
    lv_obj_set_width(toolbar, LV_PCT(100));
    lv_obj_set_height(toolbar, 56);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(toolbar, 0, 0);
    lv_obj_set_style_pad_hor(toolbar, 16, 0);       /* 左右留白 16px */
    lv_obj_set_style_pad_column(toolbar, 8, 0);     /* 控件间距 8px */
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_border_side(toolbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(toolbar, 1, 0);
    lv_obj_set_style_border_color(toolbar, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(UI_COLOR_BG_SECOND), 0);
    lv_obj_set_style_radius(toolbar, 0, 0);

    /* 工具栏标题 */
    lv_obj_t *title = lv_label_create(toolbar);
    lv_label_set_text(title, "EdgeSim AI");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, UI_FONT_DEFAULT, 0);

    /* 弹性间隔：flex_grow=1 的空容器把后续按钮推到右边 */
    lv_obj_t *spacer = lv_obj_create(toolbar);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);

    /* 导入文件按钮（次按钮样式：白底 + 边框） */
    lv_obj_t *btn_import = lv_btn_create(toolbar);
    lv_obj_set_height(btn_import, 36);
    lv_obj_set_style_pad_hor(btn_import, 12, 0);
    lv_obj_set_style_radius(btn_import, 6, 0);
    lv_obj_set_style_bg_color(btn_import, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(btn_import, 1, 0);
    lv_obj_set_style_border_color(btn_import, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(btn_import, 0, 0);
    lv_obj_t *lbl_import = lv_label_create(btn_import);
    lv_label_set_text(lbl_import, "Import");
    lv_obj_set_style_text_color(lbl_import, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(lbl_import);
    lv_obj_add_event_cb(btn_import, import_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 知识库管理按钮（阶段 6：查看已导入的知识库列表，支持搜索/排序） */
    lv_obj_t *btn_kb = lv_btn_create(toolbar);
    lv_obj_set_height(btn_kb, 36);
    lv_obj_set_style_pad_hor(btn_kb, 12, 0);
    lv_obj_set_style_radius(btn_kb, 6, 0);
    lv_obj_set_style_bg_color(btn_kb, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(btn_kb, 1, 0);
    lv_obj_set_style_border_color(btn_kb, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(btn_kb, 0, 0);
    lv_obj_t *lbl_kb = lv_label_create(btn_kb);
    lv_label_set_text(lbl_kb, "KB");
    lv_obj_set_style_text_color(lbl_kb, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(lbl_kb);
    lv_obj_add_event_cb(btn_kb, kb_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 清空对话按钮（次按钮样式） */
    lv_obj_t *btn_clear = lv_btn_create(toolbar);
    lv_obj_set_height(btn_clear, 36);
    lv_obj_set_style_pad_hor(btn_clear, 12, 0);
    lv_obj_set_style_radius(btn_clear, 6, 0);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(btn_clear, 1, 0);
    lv_obj_set_style_border_color(btn_clear, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(btn_clear, 0, 0);
    lv_obj_t *lbl_clear = lv_label_create(btn_clear);
    lv_label_set_text(lbl_clear, "Clear");
    lv_obj_set_style_text_color(lbl_clear, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(lbl_clear);
    lv_obj_add_event_cb(btn_clear, clear_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 3. 创建消息列表（滚动容器，占据主要空间） ---- */
    g_msg_list = lv_obj_create(cont);
    if (g_msg_list == NULL) {
        return -1;
    }
    lv_obj_set_flex_grow(g_msg_list, 1);
    lv_obj_set_width(g_msg_list, LV_PCT(100));
    lv_obj_set_flex_flow(g_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_msg_list, 8, 0);     /* 消息间距 8px */
    lv_obj_set_style_pad_all(g_msg_list, 16, 0);    /* 内边距 16px */
    lv_obj_set_scroll_dir(g_msg_list, LV_DIR_VER);
    lv_obj_set_style_bg_color(g_msg_list, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(g_msg_list, 0, 0);
    /* 滚动条样式：ACTIVE 模式（滚动时显示） */
    lv_obj_set_scrollbar_mode(g_msg_list, LV_SCROLLBAR_MODE_ACTIVE);

    /* ---- 4. 创建输入行（水平 flex 布局，固定高度 64px） ---- */
    lv_obj_t *input_row = lv_obj_create(cont);
    if (input_row == NULL) {
        return -1;
    }
    lv_obj_set_width(input_row, LV_PCT(100));
    lv_obj_set_height(input_row, 64);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_hor(input_row, 16, 0);
    lv_obj_set_style_pad_column(input_row, 8, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_border_side(input_row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(input_row, 1, 0);
    lv_obj_set_style_border_color(input_row, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(input_row, lv_color_hex(UI_COLOR_BG), 0);

    /* ---- 5. 创建输入文本域 ---- */
    g_ta_input = lv_textarea_create(input_row);
    if (g_ta_input == NULL) {
        return -1;
    }
    lv_obj_set_flex_grow(g_ta_input, 1);
    lv_obj_set_height(g_ta_input, 40);
    /* 中文输入支持（阶段 4.5）：输入框字体设为中文字体，
     * 否则输入法打出的中文会显示为方块 */
    lv_obj_set_style_text_font(g_ta_input, UI_FONT_CHINESE, 0);
    lv_textarea_set_placeholder_text(g_ta_input, "Type a message...");
    lv_textarea_set_max_length(g_ta_input, CHAT_INPUT_MAX_LEN);
    lv_textarea_set_one_line(g_ta_input, true);
    /* 输入框样式：圆角 8px + 浅灰背景 */
    lv_obj_set_style_radius(g_ta_input, 8, 0);
    lv_obj_set_style_border_color(g_ta_input, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(g_ta_input, lv_color_hex(UI_COLOR_BG_SECOND), 0);

    /* ---- 6. 创建发送按钮（主按钮样式：蓝色背景 + 白字） ---- */
    lv_obj_t *btn_send = lv_btn_create(input_row);
    if (btn_send == NULL) {
        return -1;
    }
    lv_obj_set_size(btn_send, 90, 40);
    lv_obj_set_style_radius(btn_send, 8, 0);
    lv_obj_set_style_bg_color(btn_send, lv_color_hex(UI_COLOR_PRIMARY), 0);
    lv_obj_set_style_shadow_width(btn_send, 0, 0);
    lv_obj_t *btn_label = lv_label_create(btn_send);
    lv_label_set_text(btn_label, "Send");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(UI_COLOR_TEXT_WHITE), 0);
    lv_obj_center(btn_label);

    /* ---- 7. 绑定事件 ---- */
    lv_obj_add_event_cb(btn_send, send_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_ta_input, send_btn_event_cb, LV_EVENT_READY, NULL);

    /* ---- 8. 注册 LLM 回复回调 与 ASR 转写回调 ----
     * ui_pipeline_set_callbacks 是累加式注册（NULL 不覆盖已注册的回调），
     * 所以这里同时传入 LLM 和 ASR 回调，不会影响 ui_mem_monitor 后续注册的 mem 回调 */
    ui_pipeline_set_callbacks(on_llm_reply_cb, on_ocr_result_cb, on_asr_result_cb, NULL);

    /* ---- 9. 欢迎消息（中文支持：需 HAS_CHINESE_FONT 才能正常显示） ---- */
    add_message_bubble("欢迎使用 EdgeSim AI 助手！", 0);

    printf("[ui_chat_window] 创建完成\n");
    return 0;
}
