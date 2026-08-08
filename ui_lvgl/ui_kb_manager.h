#ifndef UI_KB_MANAGER_H
#define UI_KB_MANAGER_H

#include "lvgl.h"

/*
 * =============================================================================
 * EdgeSim 知识库管理面板  ui_kb_manager.h
 * =============================================================================
 * 【功能】
 *   弹窗面板，展示已导入的知识库列表：
 *     - 按"目录"分组（同一目录下的文件归为一个知识库）
 *     - 每条显示：知识库名称（目录名）、文件数量、最早导入时间
 *     - 支持按导入时间排序（升/降切换）
 *     - 支持分页浏览：每页 KB_PAGE_SIZE 条，底部 [上一页] [1/N] [下一页]
 *       翻页按钮带文字标注，样式与其他按钮一致
 *   数据来源：sqlite_db 的 kb_files 表（ui_kb_manager.c 内调用 kb_query）。
 *
 * 【设计说明】
 *   与 ui_file_import（导入面板）独立：
 *     - file_import 负责"写入"（导入文件到知识库）
 *     - kb_manager  负责"查看"（列出已导入的知识库）
 *   打开面板时实时从数据库读取，无需缓存状态。
 * =============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 创建面板（初始隐藏），parent 为父容器（通常为主屏幕） */
int  ui_kb_manager_create(lv_obj_t *parent);

/* 显示面板（每次显示时从 kb_files 表刷新数据） */
int  ui_kb_manager_show(void);

/* 查询面板当前是否可见 */
int  ui_kb_manager_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_KB_MANAGER_H */
