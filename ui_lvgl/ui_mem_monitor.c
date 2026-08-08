/*
 * =============================================================================
 * EdgeSim UI 内存监控图表实现  ui_mem_monitor.c
 * =============================================================================
 * 【文件作用】
 *   创建内存监控面板，可视化展示：
 *     1. 总内存使用进度条（已用 / 上限）
 *     2. 三个 AI 模型各自占用（LLM/OCR/ASR）
 *     3. 实时数字显示（MB 与百分比）
 *   对应设计文档 1.2.1 节「硬件内存仿真管控」。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.1 节「hardware_sim（内存仿真）」
 *   EdgeSim_Design.md 第 3.5 节「ui_lvgl（界面）」
 *
 * 【数据来源】
 *   本面板不直接调用 hardware_sim，而是接收 ui_pipeline 转发的内存数据。
 *   业务层某子进程定时把 mem_usage_t 数据通过 proc_send 发给 UI 进程，
 *   ui_pipeline 收到后调用注册的 on_mem_update_cb（即本模块的回调）。
 *
 * 【LVGL 控件树】
 *   lv_obj_t *panel
 *   ├── lv_obj_t *title        ("Memory Monitor")
 *   ├── lv_obj_t *bar_total    (总进度条)
 *   ├── lv_obj_t *lbl_total    ("已用 256MB / 512MB (50%)")
 *   ├── lv_obj_t *row_llm      (LLM 行: label + bar + value)
 *   ├── lv_obj_t *row_ocr      (OCR 行)
 *   ├── lv_obj_t *row_asr      (ASR 行)
 *   ├── lv_obj_t *curve_title  ("Memory Trend (last 18s)")
 *   ├── lv_obj_t *line         (实时趋势折线，lv_line 绘制)
 *   └── lv_obj_t *lbl_peak     ("Peak: 330MB" 峰值统计)
 *
 * 【动态可视化说明（阶段 2 增强）】
 *   除进度条/数字外，本面板用 lv_line 折线绘制"最近 18 秒内存使用趋势"：
 *   - 每次数据回调（300ms 一次）把 used_mb 推入 60 点滚动窗口
 *   - 点坐标：x 均匀分布（时间轴），y = used/total 映射高度（内存轴）
 *   - 曲线随采样持续滚动，实现"任务管理器式"实时趋势图
 *   同时统计历史峰值 Peak 并实时显示。
 * =============================================================================
 */

#include "ui_lvgl.h"
#include "ui_pipeline.h"
#include <stdio.h>
#include <string.h>   /* memmove：滚动历史数组时左移 */

/* ---- 模块内部状态 ---- */
static lv_obj_t *g_panel     = NULL;   /* 监控面板主容器 */
static lv_obj_t *g_bar_total = NULL;   /* 总内存进度条 */
static lv_obj_t *g_lbl_total = NULL;   /* 总内存数值标签 */
static lv_obj_t *g_bar_llm   = NULL;   /* LLM 进度条 */
static lv_obj_t *g_lbl_llm   = NULL;   /* LLM 数值标签 */
static lv_obj_t *g_bar_ocr   = NULL;   /* OCR 进度条 */
static lv_obj_t *g_lbl_ocr   = NULL;   /* OCR 数值标签 */
static lv_obj_t *g_bar_asr   = NULL;   /* ASR 进度条 */
static lv_obj_t *g_lbl_asr   = NULL;   /* ASR 数值标签 */
static lv_obj_t *g_line      = NULL;   /* 实时趋势折线控件 */
static lv_obj_t *g_lbl_peak  = NULL;   /* 峰值统计标签 */

/* 历史采样窗口：60 点 × 300ms/点 ≈ 18 秒滚动趋势 */
#define HISTORY_MAX_POINTS  60
/* 折线绘制区域尺寸（px），与面板 320px 宽匹配（扣掉 32px 内边距） */
#define CURVE_WIDTH        288
#define CURVE_HEIGHT        64
/* 滚动窗口：存放最近 60 次 used_mb 采样值（double 支持小数） */
static double    g_hist_used[HISTORY_MAX_POINTS];
/* 折线顶点坐标数组：LVGL 的 lv_line 控件持有此数组的指针引用，
 * 因此必须是 static（全局生命周期），不能是局部变量 */
static lv_point_t g_hist_pts[HISTORY_MAX_POINTS];
static int       g_hist_count = 0;     /* 当前已采样点数（0~60） */
static double    g_peak_mb    = 0.0;   /* 历史峰值（MB，支持小数） */

/* 各模型进度条满格值（MB），即"容量上限"，用于显示占用/容量比例。
 * 【阶段 2 二次修复】满格值统一改为与全局仿真上限一致（2048MB，见
 * main.c MEM_SIM_LIMIT_MB）。原因：模型占用已改为真实文件体积
 * （LLM gguf ≈ 985MB），原 512 上限会让进度条值超出范围（溢出钳位到 100%）。
 * 2048 下显示真实比例：LLM 985/2048≈48%、OCR 60/2048≈3%、ASR 78/2048≈4%。 */
#define MEM_CAP_LLM_MB  2048
#define MEM_CAP_OCR_MB  2048
#define MEM_CAP_ASR_MB  2048


/*
 * -----------------------------------------------------------------------------
 * update_row —— 内部辅助：更新单行（label + 进度条）
 * 【参数】bar     ：进度条控件
 * 【参数】lbl     ：数值标签
 * 【参数】name    ：模型名称（"LLM" / "OCR" / "ASR"）
 * 【参数】used_mb ：已用内存
 * 【参数】cap_mb  ：该模型容量上限
 * -----------------------------------------------------------------------------
 */
static void update_row(lv_obj_t *bar, lv_obj_t *lbl,
                        const char *name, double used_mb, int cap_mb)
{
    int percent = 0;
    char buf[64];

    if (cap_mb > 0) {
        /* double 除法保留精度后再取整为百分比 */
        percent = (int)(used_mb * 100.0 / (double)cap_mb);
        if (percent > 100) percent = 100;
    }

    /* lv_bar_set_value 参数：
     *   bar    ：进度条控件
     *   value  ：当前值（范围由 lv_bar_set_range 设定）
     *   anim   ：LV_ANIM_ON=平滑动画，OFF=直接跳变
     * 【阶段 4.5 性能优化：动画 OFF】
     *   进度条动画会让 LVGL 在动画期间每帧更新并重绘进度条区域，
     *   与内存面板 1s 定时刷新叠加时增加不必要的重绘开销（卡顿源之一）。
     *   监控场景数据为阶梯变化，直接跳变即可，无需平滑过渡。 */
    /* lv_bar_set_value 接收 int32_t，double 强转取整（进度条按 MB 精度即可） */
    lv_bar_set_value(bar, (int)used_mb, LV_ANIM_OFF);

    /* 格式化数值文本：占用显示 1 位小数，贴近真实模型体积 */
    snprintf(buf, sizeof(buf), "%s: %.1fMB / %dMB (%d%%)",
             name, used_mb, cap_mb, percent);
    lv_label_set_text(lbl, buf);
}


/*
 * -----------------------------------------------------------------------------
 * update_history_curve —— 动态趋势曲线更新（阶段 2 动态可视化新增）
 * -----------------------------------------------------------------------------
 * 【参数】used_mb  ：本次采样总已用（MB）
 * 【参数】total_mb ：总上限（MB）
 * 【作用】把最新采样值推入 60 点滚动窗口，并重算折线顶点坐标后刷新 lv_line。
 * 【算法说明】
 *   1. 滚动窗口：未满时追加到末尾；已满则 memmove 整体左移 1 格，
 *      最新值写最后，窗口始终保存"最近 60 次采样"。
 *   2. 坐标映射：x 从 0 均匀铺满曲线宽度（时间轴），
 *      y = HEIGHT - used/total×HEIGHT（内存轴，used 越高曲线越靠上）。
 *   3. LVGL lv_line 直接持有 g_hist_pts 指针，更新后调用
 *      lv_line_set_points 即完成重绘，无需其他失效操作。
 *   4. 同时累计峰值 g_peak_mb，供面板实时显示。
 * -----------------------------------------------------------------------------
 */
static void update_history_curve(double used_mb, double total_mb)
{
    int i;               /* 循环索引 */
    int x, y;            /* 折线顶点坐标 */

    /* 曲线控件尚未创建（理论不会发生），安全返回 */
    if (g_line == NULL) {
        return;
    }
    /* 防御：total 为 0 时避免除零 */
    if (total_mb <= 0) {
        total_mb = 1;
    }

    /* ---- 1. 推入滚动窗口 ---- */
    if (g_hist_count < HISTORY_MAX_POINTS) {
        /* 窗口未满：直接在末尾追加 */
        g_hist_used[g_hist_count++] = used_mb;
    } else {
        /* 窗口已满：整体左移一格，腾出末尾给最新值
         * （元素类型为 double，移动字节数需按 sizeof(double)） */
        memmove(g_hist_used, g_hist_used + 1,
                (HISTORY_MAX_POINTS - 1) * sizeof(double));
        g_hist_used[HISTORY_MAX_POINTS - 1] = used_mb;
    }

    /* ---- 2. 更新峰值 ---- */
    if (used_mb > g_peak_mb) {
        g_peak_mb = used_mb;
    }

    /* ---- 3. 采样值 → 折线顶点坐标 ---- */
    for (i = 0; i < g_hist_count; i++) {
        /* x：均匀分布；减 2 留出右端边距，避免顶点贴边 */
        x = (CURVE_WIDTH - 2) * i / (HISTORY_MAX_POINTS - 1);
        /* y：按 used/total 比例映射到 0~HEIGHT，并钳位防止越界
         * （g_hist_used 已是 double，直接相除保留精度） */
        y = CURVE_HEIGHT -
            (int)(g_hist_used[i] / total_mb * CURVE_HEIGHT);
        if (y < 0)  y = 0;
        if (y > CURVE_HEIGHT) y = CURVE_HEIGHT;
        g_hist_pts[i].x = x;
        g_hist_pts[i].y = y;
    }

    /* ---- 4. 刷新折线（LVGL 自动重绘） ---- */
    lv_line_set_points(g_line, g_hist_pts, g_hist_count);
}


/*
 * -----------------------------------------------------------------------------
 * on_mem_update_cb —— 内存数据到达回调
 * 【参数】used_mb  ：总已用
 * 【参数】total_mb ：总上限
 * 【参数】llm_mb / ocr_mb / asr_mb：三模型各自占用
 * 【说明】由 ui_pipeline 在 LVGL timer 中调用，可直接操作控件。
 * -----------------------------------------------------------------------------
 */
static void on_mem_update_cb(double used_mb, double total_mb,
                              double llm_mb, double ocr_mb, double asr_mb)
{
    int percent = 0;
    char buf[64];

    if (g_panel == NULL) {
        return;
    }

    /* 总进度条：范围设为 0~total_mb，值为 used_mb
     * （lv_bar 系列 API 接收 int32_t，double 需强转）
     * 【阶段 4.5 性能优化：动画 OFF，理由见 update_row 注释】 */
    lv_bar_set_range(g_bar_total, 0, (int)(total_mb > 0 ? total_mb : 1));
    lv_bar_set_value(g_bar_total, (int)used_mb, LV_ANIM_OFF);

    /* 总数值标签：百分比用 double 计算后取整 */
    if (total_mb > 0) {
        percent = (int)(used_mb * 100.0 / total_mb);
    }
    snprintf(buf, sizeof(buf), "Total: %.1fMB / %.1fMB (%d%%)",
             used_mb, total_mb, percent);
    lv_label_set_text(g_lbl_total, buf);

    /* 接近 90% 阈值时变红警告 */
    if (percent >= 90) {
        lv_obj_set_style_bg_color(g_bar_total, lv_color_hex(0xFF5555), 0);
    } else if (percent >= 70) {
        lv_obj_set_style_bg_color(g_bar_total, lv_color_hex(0xFFAA00), 0);
    } else {
        lv_obj_set_style_bg_color(g_bar_total, lv_color_hex(0x55AA55), 0);
    }

    /* 三模型各自进度条 */
    update_row(g_bar_llm, g_lbl_llm, "LLM", llm_mb, MEM_CAP_LLM_MB);
    update_row(g_bar_ocr, g_lbl_ocr, "OCR", ocr_mb, MEM_CAP_OCR_MB);
    update_row(g_bar_asr, g_lbl_asr, "ASR", asr_mb, MEM_CAP_ASR_MB);

    /* 动态可视化：更新趋势曲线 + 峰值标签（峰值显示 1 位小数） */
    update_history_curve(used_mb, total_mb);
    snprintf(buf, sizeof(buf), "Peak: %.1fMB", g_peak_mb);
    lv_label_set_text(g_lbl_peak, buf);
}


/*
 * -----------------------------------------------------------------------------
 * create_mem_row —— 内部辅助：创建一行（标签 + 进度条）
 * 【参数】parent   ：父容器
 * 【参数】out_bar  ：输出参数，返回进度条控件指针
 * 【参数】out_lbl  ：输出参数，返回数值标签控件指针
 * 【参数】cap_mb   ：进度条上限（MB）
 * 【返回值】行容器指针
 * -----------------------------------------------------------------------------
 */
static lv_obj_t *create_mem_row(lv_obj_t *parent,
                                 lv_obj_t **out_bar, lv_obj_t **out_lbl,
                                 int cap_mb)
{
    /* 创建行容器 */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 50);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_row(row, 2, 0);

    /* 数值标签（顶部） */
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "-");

    /* 进度条
     * lv_bar_create 创建水平进度条，默认范围 0~100
     * lv_bar_set_range 修改范围；lv_bar_set_value 设置当前值 */
    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 16);
    lv_bar_set_range(bar, 0, cap_mb > 0 ? cap_mb : 1);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    if (out_bar) *out_bar = bar;
    if (out_lbl) *out_lbl = lbl;
    return row;
}


/*
 * =============================================================================
 * 公共接口：ui_mem_monitor_create
 * =============================================================================
 * 【参数】parent：父容器
 * 【返回值】0=成功，-1=失败
 * =============================================================================
 */
int ui_mem_monitor_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return -1;
    }

    /* ---- 1. 主容器 ---- */
    g_panel = lv_obj_create(parent);
    if (g_panel == NULL) {
        return -1;
    }
    /* 宽度 320px：在 1280x720 下占约 25%，对话区还有 960px */
    lv_obj_set_size(g_panel, 320, LV_PCT(100));
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_panel, 16, 0);
    lv_obj_set_style_pad_row(g_panel, 8, 0);
    lv_obj_set_style_border_width(g_panel, 0, 0);
    /* 次背景色：浅灰，与主对话区白色区分 */
    lv_obj_set_style_bg_color(g_panel, lv_color_hex(UI_COLOR_BG_SECOND), 0);

    /* ---- 2. 标题 ---- */
    lv_obj_t *title = lv_label_create(g_panel);
    lv_label_set_text(title, "Memory Monitor");
    lv_obj_set_style_text_font(title, UI_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);

    /* ---- 3. 总进度条 ---- */
    g_lbl_total = lv_label_create(g_panel);
    lv_label_set_text(g_lbl_total, "Total: -");

    g_bar_total = lv_bar_create(g_panel);
    lv_obj_set_width(g_bar_total, LV_PCT(100));
    lv_obj_set_height(g_bar_total, 20);
    lv_bar_set_range(g_bar_total, 0, 512);   /* 默认 512MB */
    lv_bar_set_value(g_bar_total, 0, LV_ANIM_OFF);
    /* 默认绿色 */
    lv_obj_set_style_bg_color(g_bar_total, lv_color_hex(0x55AA55), 0);

    /* ---- 4. 三模型行 ---- */
    create_mem_row(g_panel, &g_bar_llm, &g_lbl_llm, MEM_CAP_LLM_MB);
    create_mem_row(g_panel, &g_bar_ocr, &g_lbl_ocr, MEM_CAP_OCR_MB);
    create_mem_row(g_panel, &g_bar_asr, &g_lbl_asr, MEM_CAP_ASR_MB);

    /* ---- 4.5 动态趋势曲线（阶段 2 动态可视化）---- */
    lv_obj_t *curve_title = lv_label_create(g_panel);
    lv_label_set_text(curve_title, "Memory Trend (last 18s)");
    lv_obj_set_style_text_color(curve_title, lv_color_hex(UI_COLOR_TEXT), 0);

    /* lv_line 折线控件：点坐标由 update_history_curve 每 300ms 刷新。
     * 注意：lv_line 保存了传入点数组的指针，故 g_hist_pts 必须为 static。
     * 控件尺寸显式指定，否则默认取点包围盒大小。 */
    g_line = lv_line_create(g_panel);
    lv_obj_set_size(g_line, CURVE_WIDTH, CURVE_HEIGHT);
    lv_line_set_points(g_line, g_hist_pts, 0);   /* 初始 0 个点 */
    lv_obj_set_style_line_color(g_line, lv_color_hex(0x409EFF), 0);
    lv_obj_set_style_line_width(g_line, 2, 0);
    lv_obj_set_style_line_rounded(g_line, true, 0);

    /* 峰值统计标签 */
    g_lbl_peak = lv_label_create(g_panel);
    lv_label_set_text(g_lbl_peak, "Peak: -");
    lv_obj_set_style_text_color(g_lbl_peak, lv_color_hex(0x909399), 0);

    /* ---- 5. 注册回调 ----
     * 注意 ui_pipeline_set_callbacks 是累加式注册，
     * 这里只设置 mem_cb，其他三个 NULL。
     * 实际项目应在 ui_init 中集中调用一次，传入所有 4 个回调。
     * 此处为模块独立测试时使用，最终由 ui_main 统一注册。 */
    ui_pipeline_set_callbacks(NULL, NULL, NULL, on_mem_update_cb);

    /* ---- 6. 数据驱动（阶段 2 起）----
     * 不再自造 Mock 数据，也不再启动本地定时器。
     * 真实数据由 ui_pipeline 的 mem_refresh_timer_cb 每 300ms 驱动：
     *   hardware_sim（mem_sim_get_usage + get_model_mb）→ 本回调 →
     *   刷新进度条 + 数字 + 趋势曲线 + 峰值。
     * 因此首次显示会保持"初始 0"，模型加载后立即有真实数字，
     * 且曲线随每次采样持续滚动更新（动态监控）。 */
    printf("[ui_mem_monitor] 创建完成（动态监控：进度条+趋势曲线+峰值，300ms 刷新）\n");
    return 0;
}
