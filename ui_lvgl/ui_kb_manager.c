/*
 * =============================================================================
 * EdgeSim 知识库管理面板实现  ui_kb_manager.c
 * =============================================================================
 * 【功能】
 *   弹窗面板，展示已导入的知识库（kb_files 表）：
 *     - 按"目录"分组：同一目录下的文件归为一个知识库
 *     - 每条显示：知识库名称（单文件=文件名 / 多文件=目录名）、
 *       文件数量、最早导入时间
 *     - 分页浏览：每页 KB_PAGE_SIZE 条，[上一页] [1/N] [下一页]
 *     - 关闭按钮：隐藏面板
 *
 * 【LVGL 控件树】（阶段 6：搜索功能已删除；
 *                  阶段 7：右上角排序按钮与统计标签已删除）
 *   lv_obj_t *panel (弹窗容器, 居中, 500x400)
 *   ├── lv_obj_t *title_row        (标题行)
 *   │   └── lv_obj_t *lbl_title    ("Knowledge Base")
 *   ├── lv_obj_t *list             (滚动列表, flex_grow 占剩余空间)
 *   │   └── lv_obj_t *row_i        (名称 + N 文件 + 时间，每页 KB_PAGE_SIZE 条)
 *   └── lv_obj_t *btn_row          (翻页区 + 关闭按钮)
 *       ├── lv_obj_t *btn_prev     ("上一页" 翻页按钮，带文字标注)
 *       ├── lv_obj_t *lbl_page     ("1/3" 页码标签)
 *       ├── lv_obj_t *btn_next     ("下一页" 翻页按钮，带文字标注)
 *       └── lv_obj_t *btn_close    (关闭按钮)
 *
 * 【数据流】
 *   ui_kb_manager_show → refresh_kb_list:
 *     kb_query(全表) → 按目录分组 → 按时间排序 → 按页切片 → 重建列表行
 * =============================================================================
 */

#include "ui_kb_manager.h"
#include "ui_lvgl.h"
#include "sqlite_db.h"   /* kb_query / kb_file_t */
#include <stdio.h>
#include <string.h>

/* kb_query 输出数组容量（一次最多展示的知识库文件数）
 * 与 sqlite_db/rag 的容量约定一致（64），独立定义避免依赖 rag.h */
#define KB_FILES_MAX  64

/* ---- 模块内部状态 ---- */
static lv_obj_t *g_panel      = NULL;   /* 面板主容器 */
static lv_obj_t *g_list       = NULL;   /* 列表滚动容器 */

/* 列表固定按导入时间降序展示（最新在前）。
 * 【阶段 7 调整】右上角排序按钮已按用户要求删除，排序方向固定为降序，
 *               内部逻辑保留以便展示更合理（新导入的在前）。 */
static int g_sort_desc = 1;

/* ---- 分页状态（阶段 7 新增）----
 * 背景：知识库列表最多 64 组，全部渲染会拉长列表、滚动不便。
 * 方案：每页固定 KB_PAGE_SIZE 条，底部翻页行 [上一页] [1/N] [下一页]，
 *       翻页按钮带"上一页/下一页"文字标注，样式与其他按钮一致。
 * g_page_cur 为 0 基页码，g_page_total 为总页数（至少 1）。 */
#define KB_PAGE_SIZE    8            /* 每页显示的知识库条数 */
#define KB_PAGE_BTN_H   32           /* 翻页按钮高度（与面板其他按钮一致） */

static int  g_page_cur   = 0;        /* 当前页（0 基） */
static int  g_page_total = 1;        /* 总页数（至少 1） */
static lv_obj_t *g_btn_prev = NULL;  /* "上一页"按钮 */
static lv_obj_t *g_btn_next = NULL;  /* "下一页"按钮 */
static lv_obj_t *g_lbl_page = NULL;  /* 页码标签（"1/1"） */

/* ---- 分组数据结构 ----
 * kb_files 表是"文件级"数据（每个文件一行）。面板按"目录"分组，
 * 把同一目录下的文件归为一个知识库条目，便于用户按知识库管理。 */
#define KB_GROUP_MAX  64        /* 最多展示的知识库组数 */
#define KB_NAME_MAX   KB_PATH_MAX /* 组名长度（目录名） */

typedef struct {
    char  name[KB_NAME_MAX];       /* 分组键：目录最后一级（多文件时显示用） */
    char  first_file[KB_NAME_MAX]; /* 该组第一个文件名（单文件时显示用） */
    int   file_count;              /* 该知识库包含的文件数 */
    char  create_time[32];         /* 最早导入时间（升序展示用） */
} kb_group_t;

/* 分组结果缓存（每次 show 时重建） */
static kb_group_t g_groups[KB_GROUP_MAX];
static int        g_group_cnt = 0;


/*
 * -----------------------------------------------------------------------------
 * kb_group_find —— 在分组缓存中查找（或创建）某目录对应的分组
 * -----------------------------------------------------------------------------
 * 【参数】dir       ：分组键（目录最后一级，多文件时作为显示名）
 * 【参数】first_file：该文件的文件名（单文件时作为显示名）
 * 【参数】time      ：该文件的导入时间（用于记录最早时间）
 * 【返回值】分组下标；-1=分组已满无法创建
 * 【算法】线性查找同名分组；无则新建（记录首个文件名/时间并置 file_count=1）。
 * 【阶段 6 修复】显示名规则：单文件组显示文件名（first_file），
 *               多文件组显示目录名（name）——见 rebuild_list。
 * -----------------------------------------------------------------------------
 */
static int kb_group_find(const char *dir, const char *first_file, const char *time)
{
    int i;

    /* 1. 查找已存在的同名分组 */
    for (i = 0; i < g_group_cnt; i++) {
        if (strcmp(g_groups[i].name, dir) == 0) {
            g_groups[i].file_count++;          /* 文件数 +1 */
            /* 时间取最早：字符串 ISO 时间可直接字典序比较 */
            if (time != NULL && time[0] != '\0' &&
                strcmp(time, g_groups[i].create_time) < 0) {
                strncpy(g_groups[i].create_time, time,
                        sizeof(g_groups[i].create_time) - 1);
            }
            return i;
        }
    }

    /* 2. 新建分组 */
    if (g_group_cnt >= KB_GROUP_MAX) {
        return -1;                             /* 分组已满 */
    }
    strncpy(g_groups[g_group_cnt].name, dir, KB_NAME_MAX - 1);
    g_groups[g_group_cnt].name[KB_NAME_MAX - 1] = '\0';
    strncpy(g_groups[g_group_cnt].first_file,
            (first_file != NULL) ? first_file : dir, KB_NAME_MAX - 1);
    g_groups[g_group_cnt].first_file[KB_NAME_MAX - 1] = '\0';
    g_groups[g_group_cnt].file_count = 1;
    strncpy(g_groups[g_group_cnt].create_time,
            (time != NULL) ? time : "", sizeof(g_groups[g_group_cnt].create_time) - 1);
    g_groups[g_group_cnt].create_time[sizeof(g_groups[g_group_cnt].create_time) - 1] = '\0';
    g_group_cnt++;
    return g_group_cnt - 1;
}


/*
 * -----------------------------------------------------------------------------
 * load_groups —— 从 kb_files 表加载并按目录分组
 * -----------------------------------------------------------------------------
 * 【说明】遍历全表，提取每个文件路径的"目录最后一级"作为知识库名称，
 *         同一目录的文件合并为一条分组（文件数累加、时间取最早）。
 *         无目录（路径不含 '/'）的文件以文件名本身作为分组名。
 * -----------------------------------------------------------------------------
 */
static void load_groups(void)
{
    kb_file_t files[KB_FILES_MAX];   /* kb_files 表行（最多 64 个文件） */
    int n_files = 0;
    int i;

    g_group_cnt = 0;

    /* 查询全表；失败/为空时分组为空（面板显示"无知识库"） */
    if (kb_query(files, KB_FILES_MAX, &n_files) != 0 || n_files <= 0) {
        return;
    }

    for (i = 0; i < n_files; i++) {
        const char *path = files[i].path;
        const char *dir_name = NULL;   /* 分组键：目录最后一级 */
        const char *file_name = NULL;  /* 文件名（单文件时显示用） */
        const char *slash;

        /* 文件名 = 路径最后一个 '/' 之后的部分（无 '/' 则整个路径） */
        slash = strrchr(path, '/');
        file_name = (slash != NULL) ? slash + 1 : path;

        /* 分组键：目录最后一级（如 "/tmp/rag_demo/a.txt" → "rag_demo"） */
        if (slash != NULL && slash != path) {
            size_t dir_len = (size_t)(slash - path);
            char   dir_buf[KB_PATH_MAX];
            const char *last_slash;

            if (dir_len >= sizeof(dir_buf)) {
                dir_len = sizeof(dir_buf) - 1;
            }
            memcpy(dir_buf, path, dir_len);
            dir_buf[dir_len] = '\0';

            /* 目录名只保留最后一级（去掉上级目录前缀） */
            last_slash = strrchr(dir_buf, '/');
            dir_name = (last_slash != NULL) ? last_slash + 1 : dir_buf;
        } else {
            /* 无目录（如 "README.md"）：分组键用文件名 */
            dir_name = file_name;
        }

        /* 传入文件名：单文件组显示文件名，多文件组显示目录名 */
        kb_group_find(dir_name, file_name, files[i].create_time);
    }
}


/*
 * -----------------------------------------------------------------------------
 * rebuild_list —— 重建列表控件（应用搜索过滤 + 时间排序）
 * -----------------------------------------------------------------------------
 * 【说明】
 *   1. 先清空 g_list 的所有子对象（列表行）
 *   2. 全部分组参与排序（搜索功能已删除）
 *   3. 按 create_time 字典序降序排序（g_sort_desc 固定为降序）
 *   4. 分页计算并按当前页切片，为每行创建：名称 + N 文件 + 时间
 *   5. 更新底部翻页区（页码标签 + 按钮可用性）
 * -----------------------------------------------------------------------------
 */
static void rebuild_list(void)
{
    int   order[KB_GROUP_MAX];      /* 排序后的分组下标序列 */
    int   n_show = 0;               /* 参与排序的分组数 */
    int   i, j;                     /* 循环索引 */

    /* 1. 全部分组进入候选（搜索功能已删除，阶段 6） */
    for (i = 0; i < g_group_cnt; i++) {
        order[n_show++] = i;
    }

    /* 3. 排序：按 create_time 字典序（ISO 时间可直接比较）。
     *    简单插入排序（分组数 ≤64，性能无要求） */
    for (i = 1; i < n_show; i++) {
        int key = order[i];
        j = i - 1;
        while (j >= 0) {
            int cmp = strcmp(g_groups[order[j]].create_time,
                             g_groups[key].create_time);
            /* g_sort_desc=1 降序（大的在前）；0 升序（小的在前） */
            if (g_sort_desc ? (cmp < 0) : (cmp > 0)) {
                break;
            }
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    /* ---- 4. 分页计算（阶段 7 新增）----
     * 总页数 = ceil(n_show / KB_PAGE_SIZE)，至少 1 页；
     * 当前页越界时回退到最后一页（如数据变化后页数减少）。 */
    g_page_total = (n_show + KB_PAGE_SIZE - 1) / KB_PAGE_SIZE;
    if (g_page_total < 1) {
        g_page_total = 1;
    }
    if (g_page_cur >= g_page_total) {
        g_page_cur = g_page_total - 1;
    }

    /* 5. 清空列表并重建"当前页"的行 */
    lv_obj_clean(g_list);

    if (n_show == 0) {
        /* 空列表提示（无知识库） */
        lv_obj_t *empty = lv_label_create(g_list);
        lv_label_set_text(empty, "暂无知识库，请先导入文件");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x909399), 0);
    } else {
        /* 当前页的行下标范围 [start, end) */
        int start = g_page_cur * KB_PAGE_SIZE;
        int end   = start + KB_PAGE_SIZE;
        if (end > n_show) {
            end = n_show;
        }
        for (i = start; i < end; i++) {
            kb_group_t *g = &g_groups[order[i]];
            char  line[KB_NAME_MAX + 64];
            lv_obj_t *row;
            /* 显示名（阶段 6 修复）：
             *   单文件组 → 文件名（如 edgesim_intro.txt）
             *   多文件组 → 目录名（如 rag_demo，文件数>1 更直观） */
            const char *display_name =
                (g->file_count <= 1) ? g->first_file : g->name;

            /* 行内容：名称 + 文件数 + 时间 */
            snprintf(line, sizeof(line), "%s\n%d 个文件    %s",
                     display_name, g->file_count, g->create_time);

            row = lv_label_create(g_list);
            lv_label_set_text(row, line);
            lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(row, UI_FONT_CHINESE, 0);
            lv_obj_set_style_text_color(row, lv_color_hex(UI_COLOR_TEXT), 0);
            lv_obj_set_width(row, LV_PCT(100));
            /* 行间距 */
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
        }
    }

    /* 6. 更新底部翻页区：页码标签 + 按钮可用性
     *    首页禁用"上一页"，末页禁用"下一页"，便于用户识别边界。 */
    if (g_lbl_page != NULL) {
        char page_txt[16];
        snprintf(page_txt, sizeof(page_txt), "%d/%d",
                 g_page_cur + 1, g_page_total);
        lv_label_set_text(g_lbl_page, page_txt);
    }
    if (g_btn_prev != NULL) {
        if (g_page_cur <= 0) {
            lv_obj_add_state(g_btn_prev, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_btn_prev, LV_STATE_DISABLED);
        }
    }
    if (g_btn_next != NULL) {
        if (g_page_cur >= g_page_total - 1) {
            lv_obj_add_state(g_btn_next, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_btn_next, LV_STATE_DISABLED);
        }
    }

    /* 7. 顶部统计已删除（阶段 7）：原"右上角 N 个知识库 / N 个文件"
     * 与底部页码"1/N"信息重复，用户要求移除。 */
}


/*
 * -----------------------------------------------------------------------------
 * page_btn_cb —— 翻页按钮点击（上一页/下一页）（阶段 7 新增）
 * 【说明】点击后修改 g_page_cur 并重建列表。
 *         首/末页时按钮带 LV_STATE_DISABLED（rebuild_list 更新），
 *         LVGL 禁用态不会触发 CLICKED 事件，回调内的边界判断仅作防御。
 * -----------------------------------------------------------------------------
 */
static void page_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    (void)e;

    if (btn == g_btn_prev && g_page_cur > 0) {
        g_page_cur--;                        /* 上一页 */
    } else if (btn == g_btn_next && g_page_cur < g_page_total - 1) {
        g_page_cur++;                        /* 下一页 */
    } else {
        return;                              /* 非翻页按钮或已到边界，忽略 */
    }
    rebuild_list();                          /* 重建列表 + 刷新页码/按钮状态 */
}


/*
 * -----------------------------------------------------------------------------
 * close_btn_cb —— 关闭按钮（隐藏面板）
 * -----------------------------------------------------------------------------
 */
static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_panel != NULL) {
        lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    }
}


/*
 * =============================================================================
 * 公共接口
 * =============================================================================
 */

/*
 * ui_kb_manager_create —— 创建知识库管理面板（初始隐藏）
 */
int ui_kb_manager_create(lv_obj_t *parent)
{
    lv_obj_t *title_row, *lbl_title;
    lv_obj_t *btn_row, *btn_close;

    if (parent == NULL) {
        return -1;
    }

    /* ---- 1. 弹窗容器（居中，500x400，适配 1280x720 窗口） ----
     * 【阶段 6 修复】原 560x420 过大，Close 按钮可能被挤出可视区。
     * 调小到 500x400，且列表改用 flex_grow 占剩余空间（见下），
     * 标题/搜索/列表/关闭 四段固定布局，Close 始终可见。 */
    g_panel = lv_obj_create(parent);
    if (g_panel == NULL) {
        return -1;
    }
    lv_obj_set_size(g_panel, 500, 400);
    lv_obj_center(g_panel);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);       /* 初始隐藏 */
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_FLOATING);     /* 浮在最上层 */
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_panel, 16, 0);
    lv_obj_set_style_pad_row(g_panel, 10, 0);
    lv_obj_set_style_bg_color(g_panel, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_radius(g_panel, 12, 0);
    lv_obj_set_style_border_width(g_panel, 1, 0);
    lv_obj_set_style_border_color(g_panel, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_shadow_width(g_panel, 32, 0);
    lv_obj_set_style_shadow_opa(g_panel, LV_OPA_20, 0);

    /* ---- 2. 标题行（仅标题） ----
     * 【阶段 6 调整】搜索功能已删除；
     * 【阶段 7 调整】右上角排序按钮已删除；右上角统计标签
     *               （"N 个知识库 / N 个文件"与底部页码"1/N"信息重复）
     *               按用户要求一并删除，标题行仅保留标题。 */
    title_row = lv_obj_create(g_panel);
    lv_obj_set_size(title_row, LV_PCT(100), 36);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);

    lbl_title = lv_label_create(title_row);
    lv_label_set_text(lbl_title, "Knowledge Base");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(UI_COLOR_TEXT), 0);

    /* ---- 4. 列表滚动容器（flex column，自动滚动） ----
     * 【阶段 6 修复】用 flex_grow=1 让列表占满"标题+翻页+关闭"之外的
     * 剩余高度，而不是固定 LV_PCT(80)——固定比例在面板较小时会
     * 把底部按钮挤出可视区。flex_grow 布局下各段恒定可见。 */
    g_list = lv_obj_create(g_panel);
    lv_obj_set_width(g_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_list, 8, 0);
    lv_obj_set_style_border_color(g_list, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(g_list, 1, 0);
    lv_obj_set_style_radius(g_list, 8, 0);
    /* 容器可滚动 */
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 5. 底部按钮行（翻页按钮 + 关闭按钮）----
     * 【阶段 7】列表分页后，本行布局为：
     *   [上一页] [1/N] [下一页]  ......  [Close]
     * 翻页按钮带"上一页/下一页"文字标注，样式与其他按钮一致。
     * 注意：翻页按钮/页码直接作为 btn_row 子项（不嵌套子容器），
     *       避免多层级 flex 计算导致按钮被压缩/挤出。 */
    btn_row = lv_obj_create(g_panel);
    lv_obj_set_size(btn_row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);

    /* 5.1 "上一页"按钮（带文字标注） */
    g_btn_prev = lv_btn_create(btn_row);
    lv_obj_set_height(g_btn_prev, KB_PAGE_BTN_H);
    lv_obj_set_style_pad_hor(g_btn_prev, 10, 0);
    lv_obj_set_style_radius(g_btn_prev, 6, 0);
    lv_obj_t *lbl_prev = lv_label_create(g_btn_prev);
    lv_label_set_text(lbl_prev, "上一页");
    /* 中文标注：显式指定中文字体，避免 montserrat 缺字形显示为方块 */
    lv_obj_set_style_text_font(lbl_prev, UI_FONT_CHINESE, 0);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(lbl_prev);
    lv_obj_add_event_cb(g_btn_prev, page_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 5.2 页码标签（"1/1"，随分页刷新） */
    g_lbl_page = lv_label_create(btn_row);
    lv_label_set_text(g_lbl_page, "1/1");
    lv_obj_set_style_text_color(g_lbl_page, lv_color_hex(UI_COLOR_TEXT), 0);
    /* 固定宽度居中：页码位数变化时布局不抖动 */
    lv_obj_set_width(g_lbl_page, 44);
    lv_obj_set_style_text_align(g_lbl_page, LV_TEXT_ALIGN_CENTER, 0);

    /* 5.3 "下一页"按钮（带文字标注） */
    g_btn_next = lv_btn_create(btn_row);
    lv_obj_set_height(g_btn_next, KB_PAGE_BTN_H);
    lv_obj_set_style_pad_hor(g_btn_next, 10, 0);
    lv_obj_set_style_radius(g_btn_next, 6, 0);
    lv_obj_t *lbl_next = lv_label_create(g_btn_next);
    lv_label_set_text(lbl_next, "下一页");
    lv_obj_set_style_text_font(lbl_next, UI_FONT_CHINESE, 0);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(lbl_next);
    lv_obj_add_event_cb(g_btn_next, page_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 5.4 弹性间隔：把关闭按钮推到右侧 */
    lv_obj_t *page_spacer = lv_obj_create(btn_row);
    lv_obj_set_flex_grow(page_spacer, 1);
    lv_obj_set_height(page_spacer, 1);
    lv_obj_set_style_border_width(page_spacer, 0, 0);
    lv_obj_set_style_bg_opa(page_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page_spacer, 0, 0);

    /* 5.5 关闭按钮 */
    btn_close = lv_btn_create(btn_row);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_add_event_cb(btn_close, close_btn_cb, LV_EVENT_CLICKED, NULL);

    printf("[ui_kb_manager] 创建完成\n");
    return 0;
}


/*
 * ui_kb_manager_show —— 显示面板（每次显示重新加载数据）
 */
int ui_kb_manager_show(void)
{
    if (g_panel == NULL) {
        return -1;
    }
    /* 从 kb_files 表重新加载并按目录分组，重建列表 */
    load_groups();
    rebuild_list();
    /* 显示面板 */
    lv_obj_clear_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(g_panel);
    return 0;
}


/*
 * ui_kb_manager_is_visible —— 查询面板可见性
 */
int ui_kb_manager_is_visible(void)
{
    if (g_panel == NULL) {
        return 0;
    }
    return (lv_obj_has_flag(g_panel, LV_OBJ_FLAG_HIDDEN) == false) ? 1 : 0;
}
