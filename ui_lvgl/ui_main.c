/*
 * =============================================================================
 * EdgeSim UI 主入口实现  ui_main.c
 * =============================================================================
 * 【文件作用】
 *   实现 ui_lvgl.h 声明的 ui_init / ui_loop / ui_deinit 三个公共接口。
 *   内部完成：
 *     1. SDL2 显示驱动初始化（NO_SDL 模式下提供桩函数）
 *     2. LVGL 核心初始化与显示驱动注册
 *     3. 创建 4 个窗口模块（对话/文件导入/内存监控/悬浮窗）
 *     4. 启动 ui_pipeline 管道轮询 timer
 *     5. 提供 ui_loop 主循环与 ui_main_request_exit 退出接口
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.5 节「ui_lvgl（界面）」
 *   EdgeSim_Design.md 第 4.2 节「交叉编译」（NO_SDL 条件编译）
 *
 * 【SDL2 + LVGL 集成原理】
 *   SDL2 创建一个原生窗口，提供 framebuffer 给 LVGL 绘制；
 *   SDL2 把鼠标/键盘事件转换为 LVGL 的 lv_indev_data_t；
 *   LVGL 在 lv_timer_handler 中绘制控件到 framebuffer，SDL2 把它显示到屏幕。
 *   官方提供 sdl/iomap 的 lv_sdl_driver，本文件用简化手写版方便教学。
 * =============================================================================
 */

#include "ui_lvgl.h"
#include "ui_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   /* usleep */

/* ---- 内部状态：屏幕与显示缓冲区 ---- */
static lv_disp_draw_buf_t g_draw_buf;        /* LVGL 显示缓冲区描述符 */
static lv_color_t        *g_buf1 = NULL;     /* 显示缓冲区 1 */

/* 屏幕分辨率
 * 桌面测试用 1280x720（16:9，现代标准），看得更舒服
 * ARM64 交叉编译时可在 ui_main.c 改回 800x480 适配开发板 */
#define UI_SCREEN_WIDTH  1280
#define UI_SCREEN_HEIGHT 720

/* 退出标志：ui_main_request_exit 设为 1，ui_loop 检测后返回 -1 */
static int g_request_exit = 0;

/* 当前屏幕与主窗口对象指针（用于 show/hide） */
static lv_obj_t *g_main_cont = NULL;

/* 键盘输入设备组
 * LVGL 的键盘 indev 必须绑定到一个 group，否则按键无处可去。
 * textarea/roller 等控件创建时自动加入默认 group（lv_group_set_default）。
 * 键盘 indev 把按键发送给 group 中当前聚焦的对象。 */
static lv_group_t *g_indev_group = NULL;

/* 各窗口创建函数声明（实现在各自 .c 文件） */
extern int ui_chat_window_create(lv_obj_t *parent);
extern int ui_file_import_create(lv_obj_t *parent);
extern int ui_mem_monitor_create(lv_obj_t *parent);
extern int ui_floating_window_create(lv_obj_t *parent);

/* 知识库管理面板接口（阶段 6 新增）
 * 通过头文件引入，避免重复 extern 声明 */
#include "ui_kb_manager.h"


/* =========================================================================
 * 条件编译：SDL2 真实实现（x86 桌面环境）
 * =========================================================================
 * NO_SDL 未定义时启用，调用 SDL2 API 创建窗口、处理事件。
 * 定义 NO_SDL 时（ARM64 交叉编译）跳过 SDL 部分，仅保留 LVGL 内部逻辑。
 * ========================================================================= */
#ifndef NO_SDL

#include <SDL2/SDL.h>

/* ---- SDL2 内部状态 ---- */
static SDL_Window   *g_sdl_win   = NULL;     /* SDL 原生窗口 */
static SDL_Renderer *g_sdl_rend  = NULL;     /* SDL 渲染器 */
static SDL_Texture  *g_sdl_tex   = NULL;     /* SDL 纹理（用于显示 framebuffer） */
static int           g_sdl_init_done = 0;

/* ---- 录音状态（SDL2 音频采集）---- */
#define RECORD_MAX_SAMPLES (16000 * 30)   /* 最多录 30 秒（16kHz） */
static SDL_AudioDeviceID g_record_dev = 0; /* 录音设备 ID */
static short            *g_record_buf = NULL; /* 录音缓冲区（int16 样本数组） */
static int               g_record_len = 0;  /* 已录制的样本数 */
static int               g_recording  = 0;  /* 录音中标志 */

/* 鼠标输入设备驱动数据（被 LVGL 回调读取） */
static int32_t g_mouse_x = 0;
static int32_t g_mouse_y = 0;
static bool    g_mouse_pressed = false;

/* 键盘输入设备驱动数据（被 LVGL 回调读取）
 * 用环形队列保存按键，避免 while(SDL_PollEvent) 一次循环收到多个按键时
 * 后面的覆盖前面的导致按键丢失 */
#define KEY_QUEUE_SIZE 64
static uint32_t g_key_queue[KEY_QUEUE_SIZE];
static int      g_key_queue_head = 0;
static int      g_key_queue_tail = 0;

static void key_queue_push(uint32_t key)
{
    g_key_queue[g_key_queue_tail] = key;
    g_key_queue_tail = (g_key_queue_tail + 1) % KEY_QUEUE_SIZE;
    /* 队列满时丢弃最老的按键 */
    if (g_key_queue_tail == g_key_queue_head) {
        g_key_queue_head = (g_key_queue_head + 1) % KEY_QUEUE_SIZE;
    }
}

static uint32_t key_queue_pop(void)
{
    if (g_key_queue_head == g_key_queue_tail) {
        return 0;  /* 队列空 */
    }
    uint32_t key = g_key_queue[g_key_queue_head];
    g_key_queue_head = (g_key_queue_head + 1) % KEY_QUEUE_SIZE;
    return key;
}

static int key_queue_empty(void)
{
    return g_key_queue_head == g_key_queue_tail;
}


/*
 * -----------------------------------------------------------------------------
 * sdl_disp_flush_cb —— LVGL 显示驱动 flush 回调
 * 【参数】disp_drv：显示驱动结构体指针
 * 【参数】area    ：待刷新区域
 * 【参数】color_p ：像素数据指针
 * 【说明】LVGL 完成绘制后调用此函数，把像素数据交给 SDL 显示。
 *         完成后必须调 lv_disp_flush_ready 通知 LVGL。
 * -----------------------------------------------------------------------------
 */
static void sdl_disp_flush_cb(lv_disp_drv_t *disp_drv,
                               const lv_area_t *area, lv_color_t *color_p)
{
    (void)disp_drv;

    int area_w = area->x2 - area->x1 + 1;
    SDL_Rect rect = { area->x1, area->y1,
                      area_w,
                      area->y2 - area->y1 + 1 };
    SDL_UpdateTexture(g_sdl_tex, &rect, color_p,
                      area_w * (int)sizeof(lv_color_t));
    SDL_RenderCopy(g_sdl_rend, g_sdl_tex, NULL, NULL);
    SDL_RenderPresent(g_sdl_rend);
    lv_disp_flush_ready(disp_drv);
}


/*
 * -----------------------------------------------------------------------------
 * sdl_mouse_read_cb —— LVGL 鼠标输入设备 read 回调
 * 【参数】indev_drv：输入设备驱动
 * 【参数】data     ：输出参数，填充鼠标状态
 * -----------------------------------------------------------------------------
 */
static void sdl_mouse_read_cb(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}


/*
 * -----------------------------------------------------------------------------
 * utf8_to_unicode —— UTF-8 多字节解码为 Unicode 码点
 * 【参数】utf8：UTF-8 编码的字节串（至少 1 字节，最多 4 字节）
 * 【返回值】Unicode 码点（0 表示解码失败）
 * 【原理】UTF-8 编码规则：
 *   0xxxxxxx              → 1 字节，ASCII (0x00-0x7F)
 *   110xxxxx 10xxxxxx     → 2 字节 (0x80-0x7FF)
 *   1110xxxx 10xxxxxx 10xxxxxx → 3 字节 (0x800-0xFFFF) ← 中文常用
 *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx → 4 字节 (0x10000-0x10FFFF)
 * 【用途】SDL_TEXTINPUT 事件提供 UTF-8 字节串，LVGL 的 textarea 需要
 *         Unicode 码点才能正确显示中文（lv_textarea_add_char 接收 uint32_t）
 * -----------------------------------------------------------------------------
 */
static uint32_t utf8_to_unicode(const char *utf8)
{
    uint8_t c = (uint8_t)utf8[0];

    if (c < 0x80) {
        /* ASCII 单字节 */
        return (uint32_t)c;
    } else if ((c & 0xE0) == 0xC0 && utf8[1]) {
        /* 2 字节 UTF-8 */
        return ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)utf8[1] & 0x3F);
    } else if ((c & 0xF0) == 0xE0 && utf8[1] && utf8[2]) {
        /* 3 字节 UTF-8（中文字符主要在此范围） */
        return ((uint32_t)(c & 0x0F) << 12) |
               ((uint32_t)((uint8_t)utf8[1] & 0x3F) << 6) |
               ((uint8_t)utf8[2] & 0x3F);
    } else if ((c & 0xF8) == 0xF0 && utf8[1] && utf8[2] && utf8[3]) {
        /* 4 字节 UTF-8（emoji 等） */
        return ((uint32_t)(c & 0x07) << 18) |
               ((uint32_t)((uint8_t)utf8[1] & 0x3F) << 12) |
               ((uint32_t)((uint8_t)utf8[2] & 0x3F) << 6) |
               ((uint8_t)utf8[3] & 0x3F);
    }
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * sdl_keyboard_read_cb —— LVGL 键盘输入设备 read 回调
 * 【参数】indev_drv：输入设备驱动
 * 【参数】data     ：输出参数，填充按键状态
 * 【说明】LVGL 在每次 indev 周期调用此函数读取按键。
 *         data->key 是 Unicode 码点或 LV_KEY_* 常量
 *         data->state 是 PRESSED/RELEASED
 *         textarea 收到按键后会调 lv_textarea_add_char 或执行退格/方向等操作
 * -----------------------------------------------------------------------------
 */
static void sdl_keyboard_read_cb(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;
    if (!key_queue_empty()) {
        data->key = key_queue_pop();
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}


/*
 * -----------------------------------------------------------------------------
 * sdl_init —— 初始化 SDL2（窗口、渲染器、纹理）
 * 【参数】argc/argv：从 main 传入，SDL_init 需要
 * 【返回值】0=成功，-1=失败
 * -----------------------------------------------------------------------------
 */
static int sdl_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* SDL_Init 初始化 SDL 子系统
     * SDL_INIT_VIDEO：视频子系统（窗口、渲染） */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[ui_main] SDL_Init 失败: %s\n", SDL_GetError());
        return -1;
    }

    /* 创建窗口：位置默认，尺寸 800x480，标记为 shown */
    g_sdl_win = SDL_CreateWindow("EdgeSim",
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT,
                                  SDL_WINDOW_SHOWN);
    if (g_sdl_win == NULL) {
        printf("[ui_main] SDL_CreateWindow 失败: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    /* 创建渲染器（硬件加速优先） */
    g_sdl_rend = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_ACCELERATED);
    if (g_sdl_rend == NULL) {
        printf("[ui_main] SDL_CreateRenderer 失败，回退软件渲染\n");
        g_sdl_rend = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_SOFTWARE);
        if (g_sdl_rend == NULL) {
            printf("[ui_main] 软件渲染也失败: %s\n", SDL_GetError());
            SDL_DestroyWindow(g_sdl_win);
            SDL_Quit();
            return -1;
        }
    }

    /* 创建纹理：用于显示 LVGL 输出的像素
     * SDL_TEXTUREACCESS_STREAMING 表示纹理可被 CPU 读写更新 */
    g_sdl_tex = SDL_CreateTexture(g_sdl_rend,
                                   SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    if (g_sdl_tex == NULL) {
        printf("[ui_main] SDL_CreateTexture 失败: %s\n", SDL_GetError());
        SDL_DestroyRenderer(g_sdl_rend);
        SDL_DestroyWindow(g_sdl_win);
        SDL_Quit();
        return -1;
    }

    g_sdl_init_done = 1;

    /* 启用 SDL 文本输入模式
     * 默认情况下 SDL2 不发送 SDL_TEXTINPUT 事件
     * 调用 SDL_StartTextInput 后，每次按键都会产生 TEXTINPUT 事件
     * 携带 UTF-8 编码的字符（支持中文输入法） */
    SDL_StartTextInput();

    printf("[ui_main] SDL2 初始化完成 (%dx%d)\n",
           UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * dispatch_key —— 键盘事件分发：按面板可见性决定接收者
 * 【参数】c：Unicode 码点或 LV_KEY_* 常量
 * 【说明】文件导入面板可见时，键盘输入发给路径输入框；
 *         否则发给聊天窗口的输入框。这样两个输入框不会抢键盘。
 * -----------------------------------------------------------------------------
 */
static void dispatch_key(uint32_t c)
{
    /* 分发优先级：文件导入面板 > 聊天输入框
     * （kb_manager 为只读列表，无输入控件，不参与键盘分发） */
    if (ui_file_import_is_visible()) {
        ui_file_import_handle_char(c);
    } else {
        ui_chat_window_handle_char(c);
    }
}

/*
 * dispatch_paste —— 粘贴事件分发：按面板可见性决定接收者
 */
static void dispatch_paste(const char *text)
{
    if (ui_file_import_is_visible()) {
        ui_file_import_paste(text);
    } else {
        ui_chat_window_paste(text);
    }
}


/*
 * -----------------------------------------------------------------------------
 * sdl_process_events —— 处理 SDL 事件队列
 * 【返回值】0=继续，-1=收到退出事件
 * -----------------------------------------------------------------------------
 */
static int sdl_process_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            /* 用户点窗口关闭按钮 */
            return -1;

        case SDL_MOUSEMOTION:
            /* 鼠标移动：更新坐标 */
            g_mouse_x = ev.motion.x;
            g_mouse_y = ev.motion.y;
            break;

        case SDL_MOUSEBUTTONDOWN:
            /* 鼠标按下：更新坐标 + 状态
             * SDL_BUTTON_LEFT = 1 */
            if (ev.button.button == SDL_BUTTON_LEFT) {
                g_mouse_x = ev.button.x;
                g_mouse_y = ev.button.y;
                g_mouse_pressed = true;
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                g_mouse_x = ev.button.x;
                g_mouse_y = ev.button.y;
                g_mouse_pressed = false;
            }
            break;

        case SDL_KEYDOWN: {
            /* 键盘按下：映射到 LVGL 按键码 */
            SDL_Keycode sym = ev.key.keysym.sym;

            /* ESC 键退出程序 */
            if (sym == SDLK_ESCAPE) {
                return -1;
            }

            /* Ctrl+V 粘贴：从系统剪贴板读取文本到输入框 */
            if ((ev.key.keysym.mod & KMOD_CTRL) && (sym == SDLK_v)) {
#ifndef NO_SDL
                char *clip = SDL_GetClipboardText();
                if (clip != NULL && clip[0] != '\0') {
                    dispatch_paste(clip);
                    printf("[ui_main] 粘贴: %.50s...\n", clip);
                }
                SDL_free(clip);
#endif
                break;  /* 不走后面的 LVGL 按键处理 */
            }

            /* 特殊键直接处理，普通可打印字符由 SDL_TEXTINPUT 处理
             * 通过 dispatch_key 按面板可见性分发，绕过 LVGL indev 30ms 读取周期 */
            switch (sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                dispatch_key(LV_KEY_ENTER);
                break;
            case SDLK_BACKSPACE:
                dispatch_key(LV_KEY_BACKSPACE);
                break;
            case SDLK_DELETE:
                dispatch_key(LV_KEY_DEL);
                break;
            case SDLK_LEFT:
                dispatch_key(LV_KEY_LEFT);
                break;
            case SDLK_RIGHT:
                dispatch_key(LV_KEY_RIGHT);
                break;
            case SDLK_UP:
                dispatch_key(LV_KEY_UP);
                break;
            case SDLK_DOWN:
                dispatch_key(LV_KEY_DOWN);
                break;
            case SDLK_HOME:
                dispatch_key(LV_KEY_HOME);
                break;
            case SDLK_END:
                dispatch_key(LV_KEY_END);
                break;
            default:
                /* 可打印 ASCII 字符交给 TEXTINPUT 事件处理（支持中文输入法） */
                break;
            }
            break;
        }

        case SDL_KEYUP:
            /* 键盘松开：队列模式下无需处理（read_cb 自动返回 RELEASED） */
            break;

        case SDL_TEXTINPUT:
            /* SDL_TEXTINPUT 提供 UTF-8 编码文本（支持中文输入法）。
             * 【阶段 4.5 修复：逐字符解码整串】
             * SDL2 的 TEXTINPUT 事件 text 字段可能一次携带多个 UTF-8 字符：
             *   中文输入法"词组/整句上屏"时，一次提交多个汉字
             *   （如输入"你好"选词上屏，text="你好"共 6 字节）。
             * 旧代码 utf8_to_unicode 只解码第 1 个字符，导致词组只进 1 个字、
             * 其余丢失（表现为"键盘输中文失败，但粘贴整串正常"）。
             * 修复：循环遍历整串，逐字符解码后依次 dispatch_key 分发。
             * 【前进步长】按 UTF-8 首字节高位判断当前字符占几个字节：
             *   <0x80  → 1 字节（ASCII）
             *   <0xE0  → 2 字节
             *   <0xF0  → 3 字节（汉字）
             *   否则   → 4 字节（emoji 等扩展字符）
             */
            {
                const char *p = ev.text.text;   /* 扫描指针，逐字符前进 */
                while (*p != '\0') {
                    uint32_t cp = utf8_to_unicode(p);   /* 解码当前字符 */
                    if (cp > 0) {
                        dispatch_key(cp);               /* 分发到当前输入框 */
                    }
                    /* 按首字节高位前进到下一字符 */
                    p += ((uint8_t)*p < 0x80) ? 1
                       : ((uint8_t)*p < 0xE0) ? 2
                       : ((uint8_t)*p < 0xF0) ? 3 : 4;
                }
            }
            break;

        case SDL_MOUSEWHEEL:
            /* 鼠标滚轮：滚动对话消息列表
             * ev.wheel.y > 0 = 滚轮向上（看上面的内容）→ scroll_by dy 为正
             * ev.wheel.y < 0 = 滚轮向下（看下面的内容）→ scroll_by dy 为负
             * 每次滚动 40 像素 */
            ui_chat_window_scroll(ev.wheel.y * 40);
            break;

        default:
            break;
        }
    }
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * sdl_deinit —— 释放 SDL2 资源
 * -----------------------------------------------------------------------------
 */
static void sdl_deinit(void)
{
    if (g_sdl_tex)   { SDL_DestroyTexture(g_sdl_tex);   g_sdl_tex = NULL; }
    if (g_sdl_rend)  { SDL_DestroyRenderer(g_sdl_rend); g_sdl_rend = NULL; }
    if (g_sdl_win)   { SDL_DestroyWindow(g_sdl_win);    g_sdl_win = NULL; }
    if (g_sdl_init_done) {
        SDL_Quit();
        g_sdl_init_done = 0;
    }
}


/*
 * =============================================================================
 * 录音功能（SDL2 音频采集，供 ASR 引擎使用）
 * =============================================================================
 * 【流程】
 *   1. ui_main_start_recording：打开默认录音设备，开始采集
 *   2. ui_main_poll_recording：在 ui_loop 中轮询读取音频数据到缓冲区
 *   3. ui_main_stop_recording：停止采集，保存为 WAV 文件
 *
 * 【SDL2 录音 API】
 *   SDL_OpenAudioDevice(NULL, 1, &want, &have, 0)  1=capture
 *   SDL_PauseAudioDevice(dev, 0)                    0=开始采集
 *   SDL_GetQueuedAudioSize(dev, 1)                  获取缓冲区数据量
 *   SDL_DequeueAudio(dev, buf, size)                读取音频数据
 *   SDL_PauseAudioDevice(dev, 1)                    1=暂停
 *   SDL_CloseAudioDevice(dev)                       关闭设备
 * =============================================================================
 */

/*
 * write_wav_file —— 将 int16 样本数组写入 16kHz 单声道 PCM WAV 文件
 * 【参数】path：输出文件路径
 * 【参数】samples：样本数组
 * 【参数】n_samples：样本数
 * 【参数】sample_rate：采样率（通常 16000）
 */
static void write_wav_file(const char *path, const short *samples,
                            int n_samples, int sample_rate)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        printf("[ui_main] WAV 文件创建失败: %s\n", path);
        return;
    }
    int data_size = n_samples * 2;     /* 16-bit = 2 字节/样本 */
    int total_size = 36 + data_size;   /* RIFF 总大小 */
    short format = 1;                  /* PCM */
    short channels = 1;                /* 单声道 */
    int byte_rate = sample_rate * channels * 2;
    short block_align = channels * 2;
    short bits = 16;

    /* RIFF 头 */
    fwrite("RIFF", 1, 4, fp);
    fwrite(&total_size, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);
    /* fmt 子块 */
    fwrite("fmt ", 1, 4, fp);
    int fmt_size = 16;
    fwrite(&fmt_size, 4, 1, fp);
    fwrite(&format, 2, 1, fp);
    fwrite(&channels, 2, 1, fp);
    fwrite(&sample_rate, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp);
    fwrite(&bits, 2, 1, fp);
    /* data 子块 */
    fwrite("data", 1, 4, fp);
    fwrite(&data_size, 4, 1, fp);
    fwrite(samples, 2, n_samples, fp);

    fclose(fp);
}

int ui_main_start_recording(void)
{
    SDL_AudioSpec want, have;

    if (g_recording) {
        printf("[ui_main] 已在录音中\n");
        return -1;
    }

    /* 初始化 SDL 音频子系统 */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("[ui_main] SDL 音频子系统初始化失败: %s\n", SDL_GetError());
        return -1;
    }

    /* 配置录音参数：16kHz 单声道 16-bit PCM（Whisper 要求） */
    SDL_zero(want);
    want.freq     = 16000;       /* 采样率 16kHz */
    want.format   = AUDIO_S16;   /* 16-bit 有符号整数 */
    want.channels = 1;           /* 单声道 */
    /* 【阶段 4.5 优化：samples 4096 → 1024】
     * samples 是 SDL 音频缓冲的帧数，直接影响录音延迟：
     *   4096 帧 → 256ms 缓冲（录音响应慢，期间数据积压多）
     *   1024 帧 →  64ms 缓冲（录音响应快，队列积压小）
     * 更小的缓冲让 ui_main_poll_recording 每次取到的数据量更少、
     * 更均匀，配合 poll 节流（50ms）不会丢数据（50ms×16kHz=800
     * 样本 < 1024 缓冲，安全）。 */
    want.samples  = 1024;        /* 缓冲区大小（帧数） */
    want.callback = NULL;        /* 不用回调，用 SDL_DequeueAudio 轮询 */

    /* 打开默认录音设备（参数 1 = capture） */
    g_record_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (g_record_dev == 0) {
        printf("[ui_main] 录音设备打开失败: %s\n", SDL_GetError());
        return -1;
    }

    /* 分配录音缓冲区 */
    if (g_record_buf == NULL) {
        g_record_buf = (short *)malloc(RECORD_MAX_SAMPLES * sizeof(short));
        if (g_record_buf == NULL) {
            printf("[ui_main] 录音缓冲区分配失败\n");
            SDL_CloseAudioDevice(g_record_dev);
            g_record_dev = 0;
            return -1;
        }
    }
    g_record_len = 0;
    g_recording = 1;

    /* 开始采集（0 = 恢复） */
    SDL_PauseAudioDevice(g_record_dev, 0);
    printf("[ui_main] 开始录音（16kHz 单声道，最多 %d 秒）\n",
           RECORD_MAX_SAMPLES / 16000);
    return 0;
}

void ui_main_poll_recording(void)
{
    if (!g_recording || g_record_dev == 0) return;

    /* 【阶段 4.5 优化：poll 节流】
     * ui_loop 每 ~10ms 调用本函数（100 次/秒）。录音期间频繁调用
     * SDL_GetQueuedAudioSize / SDL_DequeueAudio 会带来不必要的主循环
     * 开销（在弱虚拟机上叠加中文字体渲染更明显）。
     * 节流：两次 poll 间隔至少 50ms（20 次/秒）。16kHz 下 50ms 最多
     *       积压 800 样本（1.6KB），远小于 1024 帧音频缓冲，不丢数据。
     * SDL_GetPerformanceCounter 为单调时钟，不受系统时间调整影响。 */
    {
        static Uint64 s_last_us = 0;   /* 上次 poll 的时钟计数（static 保持跨调用） */
        Uint64 now  = SDL_GetPerformanceCounter();
        Uint64 freq = SDL_GetPerformanceFrequency();
        if (freq > 0 && now - s_last_us < freq / 20) {
            return;                    /* 距上次不足 50ms，跳过本轮 */
        }
        s_last_us = now;
    }

    /* 获取录音设备缓冲区中可用的数据量（字节）
     * SDL_GetQueuedAudioSize 只接受 1 个参数（设备 ID）
     * 对录音设备返回已采集但尚未 dequeue 的字节数 */
    Uint32 avail = SDL_GetQueuedAudioSize(g_record_dev);
    if (avail == 0) return;

    int samples_avail = avail / 2;   /* 16-bit = 2 字节/样本 */
    int space = RECORD_MAX_SAMPLES - g_record_len;
    if (samples_avail > space) {
        samples_avail = space;
    }
    if (samples_avail <= 0) {
        /* 缓冲区满，停止录音 */
        printf("[ui_main] 录音缓冲区已满\n");
        return;
    }

    /* 从设备队列读取音频数据到缓冲区 */
    SDL_DequeueAudio(g_record_dev, g_record_buf + g_record_len,
                     samples_avail * 2);
    g_record_len += samples_avail;
}

int ui_main_stop_recording(const char *output_path)
{
    if (!g_recording) {
        return -1;
    }

    /* 暂停并关闭录音设备 */
    SDL_PauseAudioDevice(g_record_dev, 1);
    SDL_CloseAudioDevice(g_record_dev);
    g_record_dev = 0;
    g_recording = 0;

    /* 排空剩余数据 */
    ui_main_poll_recording();

    /* 保存为 WAV 文件 */
    if (output_path != NULL && g_record_buf != NULL && g_record_len > 0) {
        write_wav_file(output_path, g_record_buf, g_record_len, 16000);
        printf("[ui_main] 录音保存: %s（%d 样本，%.1f 秒）\n",
               output_path, g_record_len, (float)g_record_len / 16000.0f);
        return 0;
    }

    printf("[ui_main] 录音停止但无数据\n");
    return -1;
}

int ui_main_is_recording(void)
{
    return g_recording;
}


/*
 * -----------------------------------------------------------------------------
 * ui_main_take_screenshot —— 截取当前屏幕画面，保存为 24 位 BMP 文件
 * -----------------------------------------------------------------------------
 * 【参数】path：BMP 文件保存路径（如 "/tmp/edgesim_screenshot.bmp"）
 * 【返回值】0=成功，-1=失败
 * 【原理】
 *   1. 用 SDL_RenderReadPixels 从渲染器读取整个画面像素（ARGB8888 格式）
 *   2. 手动写 BMP 文件头（14 字节）+ DIB 头（40 字节）+ 像素数据
 *   3. BMP 像素格式为 BGR（24 位），每行 4 字节对齐，从下到上存储
 *   4. SDL 的 ARGB8888 格式：A=R[3] G=R[2] B=R[1] R=R[0]，需转为 BGR
 * 【说明】选 BMP 而非 PNG：BMP 无需第三方库，OpenCV/OCR 引擎都能读取
 * -----------------------------------------------------------------------------
 */
int ui_main_take_screenshot(const char *path)
{
    /* SDL 渲染器必须已初始化 */
    if (g_sdl_rend == NULL || !g_sdl_init_done) {
        printf("[ui_main] 截图失败：SDL 渲染器未初始化\n");
        return -1;
    }

    /* 分配像素缓冲区：4 字节/像素 (ARGB8888)
     * 屏幕分辨率 UI_SCREEN_WIDTH × UI_SCREEN_HEIGHT */
    int w = UI_SCREEN_WIDTH;
    int h = UI_SCREEN_HEIGHT;
    uint32_t *pixels = (uint32_t *)malloc(w * h * 4);
    if (pixels == NULL) {
        printf("[ui_main] 截图失败：内存分配失败\n");
        return -1;
    }

    /* 从渲染器读取像素
     * SDL_PIXELFORMAT_ARGB8888：每像素 4 字节，A 在高位，R/G/B 依次
     * NULL 矩形 = 读取整个渲染目标 */
    int ret = SDL_RenderReadPixels(g_sdl_rend, NULL,
                                    SDL_PIXELFORMAT_ARGB8888,
                                    pixels, w * 4);
    if (ret != 0) {
        printf("[ui_main] 截图失败：SDL_RenderReadPixels 错误: %s\n", SDL_GetError());
        free(pixels);
        return -1;
    }

    /* 打开 BMP 文件 */
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        printf("[ui_main] 截图失败：无法创建文件 %s\n", path);
        free(pixels);
        return -1;
    }

    /* BMP 每行需 4 字节对齐
     * 24 位 BMP 每行字节数 = w * 3，补齐到 4 的倍数 */
    int row_bytes = (w * 3 + 3) & ~3;   /* 对齐后的每行字节数 */
    int padding   = row_bytes - w * 3;  /* 每行末尾的填充字节数 */
    int img_size  = row_bytes * h;      /* 像素数据总大小 */
    int file_size = 14 + 40 + img_size; /* 文件总大小 = 头 + 信息 + 数据 */

    /* ---- BMP 文件头（14 字节）---- */
    uint8_t file_hdr[14] = {0};
    file_hdr[0] = 'B'; file_hdr[1] = 'M';           /* 文件类型标识 "BM" */
    file_hdr[2] = file_size & 0xFF;                  /* 文件大小（4 字节，小端） */
    file_hdr[3] = (file_size >> 8) & 0xFF;
    file_hdr[4] = (file_size >> 16) & 0xFF;
    file_hdr[5] = (file_size >> 24) & 0xFF;
    file_hdr[10] = 14 + 40;                          /* 像素数据偏移量 = 文件头 + 信息头 */
    fwrite(file_hdr, 1, 14, fp);

    /* ---- BMP 信息头（DIB 头，40 字节）---- */
    uint8_t info_hdr[40] = {0};
    info_hdr[0] = 40;                                /* 信息头大小 */
    info_hdr[4] = w & 0xFF;                          /* 宽度（4 字节，小端） */
    info_hdr[5] = (w >> 8) & 0xFF;
    info_hdr[6] = (w >> 16) & 0xFF;
    info_hdr[7] = (w >> 24) & 0xFF;
    info_hdr[8]  = h & 0xFF;                         /* 高度（4 字节，小端，正数=从下到上） */
    info_hdr[9]  = (h >> 8) & 0xFF;
    info_hdr[10] = (h >> 16) & 0xFF;
    info_hdr[11] = (h >> 24) & 0xFF;
    info_hdr[12] = 1;                                /* 颜色平面数 = 1 */
    info_hdr[14] = 24;                               /* 位深度 = 24 (BGR) */
    info_hdr[20] = img_size & 0xFF;                  /* 像素数据大小（4 字节，小端） */
    info_hdr[21] = (img_size >> 8) & 0xFF;
    info_hdr[22] = (img_size >> 16) & 0xFF;
    info_hdr[23] = (img_size >> 24) & 0xFF;
    info_hdr[24] = 0x13; info_hdr[25] = 0x0B;        /* 水平分辨率 2835 像素/米 (72 DPI) */
    info_hdr[28] = 0x13; info_hdr[29] = 0x0B;        /* 垂直分辨率 2835 像素/米 (72 DPI) */
    fwrite(info_hdr, 1, 40, fp);

    /* ---- 像素数据（BGR 24 位，从下到上）----
     * SDL 的 ARGB8888 格式：每个 uint32_t = 0xAARRGGBB
     *   字节 0 = R, 字节 1 = G, 字节 2 = B, 字节 3 = A
     * BMP 24 位格式：每个像素 3 字节 = B, G, R（注意顺序反转）
     * BMP 从下到上存储：最后一行在前 */
    uint8_t pad[3] = {0, 0, 0};  /* 行末填充字节 */
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t px = pixels[y * w + x];
            uint8_t bgr[3];
            bgr[0] = (px >> 0) & 0xFF;   /* B */
            bgr[1] = (px >> 8) & 0xFF;   /* G */
            bgr[2] = (px >> 16) & 0xFF;  /* R */
            fwrite(bgr, 1, 3, fp);
        }
        /* 每行末尾填充到 4 字节对齐 */
        if (padding > 0) {
            fwrite(pad, 1, padding, fp);
        }
    }

    fclose(fp);
    free(pixels);

    printf("[ui_main] 截图已保存: %s (%dx%d)\n", path, w, h);
    return 0;
}


/* =========================================================================
 * 条件编译：NO_SDL 桩实现（ARM64 交叉编译，开发板无桌面）
 * ========================================================================= */
#else  /* NO_SDL */

static int sdl_init(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[ui_main] NO_SDL 模式，跳过 SDL2 初始化\n");
    return 0;
}

static int sdl_process_events(void)
{
    /* 无 SDL，不处理事件 */
    return 0;
}

static void sdl_deinit(void)
{
    /* 无操作 */
}

/* NO_SDL 模式下录音函数的桩实现 */
int ui_main_start_recording(void) { return -1; }
void ui_main_poll_recording(void) { }
int ui_main_stop_recording(const char *p) { (void)p; return -1; }
int ui_main_is_recording(void) { return 0; }
int ui_main_take_screenshot(const char *p) { (void)p; return -1; }

#endif /* NO_SDL */


/*
 * =============================================================================
 * 公共辅助函数
 * =============================================================================
 */

/*
 * ui_clipboard_copy —— 复制文本到系统剪贴板
 * 【参数】text：要复制的文本（NULL 结尾）
 * 【说明】在 SDL2 模式下调用 SDL_SetClipboardText；
 *         NO_SDL 模式下打印日志（开发板无桌面剪贴板）
 */
void ui_clipboard_copy(const char *text)
{
    if (text == NULL) return;
#ifndef NO_SDL
    if (g_sdl_init_done && g_sdl_win != NULL) {
        SDL_SetClipboardText(text);
        printf("[ui_main] 已复制到剪贴板: %.50s...\n", text);
    }
#else
    printf("[ui_main] NO_SDL 模式，无法复制到剪贴板\n");
#endif
}


/*
 * =============================================================================
 * 公共接口实现：ui_init / ui_loop / ui_deinit
 * =============================================================================
 */

int ui_init(int argc, char **argv)
{
    /* ---- 显示/输入驱动结构体（必须 static！） ----
     * 【易错点·核心】lv_disp_drv_register / lv_indev_drv_register 内部
     *   会把这里的地址长期保存到 LVGL 的 disp_refr->driver 指针中，
     *   后续每次 lv_timer_handler → lv_refr_areas → lv_refr_area 都会
     *   通过该指针回访 driver->draw_buf->buf_act。
     *   若写成普通局部变量，ui_init 一返回栈帧即销毁，
     *   disp_refr->driver 立刻变成悬空指针，
     *   下一次刷新在 lv_refr.c:525 处解引用就触发 SIGSEGV。
     *   改为 static 后生命周期延伸到整个进程，指针长期有效。 */
    static lv_disp_drv_t disp_drv;          /* 显示驱动结构体（static，避免悬空） */
    static lv_indev_drv_t indev_drv_mouse;  /* 鼠标输入驱动（必须独立 static，不能和键盘复用） */
    static lv_indev_drv_t indev_drv_key;    /* 键盘输入驱动（LVGL 内部保存 &indev_drv 长期引用） */

    /* ---- 1. 初始化 SDL2（NO_SDL 模式下是桩） ---- */
    if (sdl_init(argc, argv) != 0) {
        return -1;
    }

    /* ---- 2. LVGL 核心初始化 ----
     * lv_init 必须最先调用，初始化 LVGL 内部数据结构 */
    lv_init();

    /* ---- 3. 分配显示缓冲区 ----
     * 全屏单缓冲：桌面环境内存充足，用整屏缓冲区最稳定
     * （1/10 屏双缓冲在 v8.2 的分块刷新路径下有内存越界风险） */
    g_buf1 = malloc(UI_SCREEN_WIDTH * UI_SCREEN_HEIGHT * sizeof(lv_color_t));
    if (g_buf1 == NULL) {
        printf("[ui_main] 显示缓冲区分配失败\n");
        sdl_deinit();
        return -1;
    }

    /* lv_disp_draw_buf_init 初始化显示缓冲区描述符
     * 参数：描述符, buf1, buf2(NULL=单缓冲), 缓冲区像素数 */
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, NULL,
                           UI_SCREEN_WIDTH * UI_SCREEN_HEIGHT);

    /* ---- 4. 注册显示驱动 ---- */
    lv_disp_drv_init(&disp_drv);                  /* 先初始化默认值 */
    disp_drv.hor_res  = UI_SCREEN_WIDTH;          /* 水平分辨率 */
    disp_drv.ver_res  = UI_SCREEN_HEIGHT;         /* 垂直分辨率 */
    disp_drv.draw_buf = &g_draw_buf;              /* 显示缓冲区 */
    /* 部分刷新模式：只重绘 dirty 区域，不用每次复制全屏 3.7MB 数据
     * RenderPresent 只在最后一次 flush 时调（lv_disp_flush_is_last 判断），
     * 避免多个区域多次等 VSync */
#ifndef NO_SDL
    disp_drv.flush_cb = sdl_disp_flush_cb;
#else
    disp_drv.flush_cb = NULL;
#endif
    lv_disp_drv_register(&disp_drv);              /* 注册到 LVGL */

    /* ---- 5. 注册鼠标输入设备 ---- */
    lv_indev_drv_init(&indev_drv_mouse);
    indev_drv_mouse.type    = LV_INDEV_TYPE_POINTER;    /* 指针类型（鼠标/触摸） */
#ifndef NO_SDL
    indev_drv_mouse.read_cb = sdl_mouse_read_cb;
#else
    indev_drv_mouse.read_cb = NULL;
#endif
    lv_indev_drv_register(&indev_drv_mouse);

    /* ---- 5.1 注册键盘输入设备 ----
     * LV_INDEV_TYPE_KEYPAD 类型，read_cb 返回按键码和状态
     * textarea 等可编辑控件会自动接收键盘输入（无需手动绑定）
     * 按键流程：SDL_KEYDOWN/TEXTINPUT → g_key_val/g_key_pressed →
     *           sdl_keyboard_read_cb → LVGL indev → textarea */
    lv_indev_drv_init(&indev_drv_key);
    indev_drv_key.type    = LV_INDEV_TYPE_KEYPAD;
#ifndef NO_SDL
    indev_drv_key.read_cb = sdl_keyboard_read_cb;
#else
    indev_drv_key.read_cb = NULL;
#endif
    lv_indev_t *key_indev = lv_indev_drv_register(&indev_drv_key);

    /* ---- 5.2 创建键盘输入组并绑定 ----
     * LVGL 键盘 indev 必须绑定 group，按键才能送到控件
     * lv_group_set_default 后，textarea/roller 等控件自动加入此 group
     * lv_indev_set_group 把键盘 indev 和 group 绑定 */
    g_indev_group = lv_group_create();
    lv_group_set_default(g_indev_group);
    lv_indev_set_group(key_indev, g_indev_group);

    /* ---- 6. 创建主屏幕与子容器 ----
     * lv_scr_act 返回当前活动屏幕对象 */
    lv_obj_t *scr = lv_scr_act();
    /* 屏幕背景：主背景色（浅色主题为纯白） */
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    /* 设置默认字体为中文字体
     * 所有未显式指定字体的控件会继承屏幕的字体 */
    lv_obj_set_style_text_font(scr, UI_FONT_DEFAULT, 0);

    /* 【中文字体 fallback 链（阶段 4.5 修复）】
     * 场景：lv_font_chinese_16 由 lv_font_conv 生成，只收录汉字
     *       （GB2312 一级 3755 字）；ASCII（英文/数字）字形在生成时
     *       不可靠（--range/--symbols 均实测会丢 ASCII），且尝试
     *       全量 CJK 会超过 LVGL 8.2 的 1MB bitmap 位域限制。
     * 方案：LVGL 字体支持 fallback 链——当前字体找不到字形时自动
     *       查询 fallback 字体。把 ASCII 回退到 LVGL 内置
     *       montserrat_14（含完整 ASCII 字形），于是：
     *         汉字   → lv_font_chinese_16 直接命中
     *         英文/数字/符号 → 自动 fallback 到 montserrat_14
     *       UI_FONT_CHINESE 指向的是非 const 的 lv_font_chinese_16
     *       （lv_font_conv 生成），故可运行时修改其 fallback 字段。
     *       仅当 HAS_CHINESE_FONT 生效（字体文件存在）时设置，
     *       否则 UI_FONT_CHINESE 就是 montserrat 本身，避免自指。 */
#ifdef HAS_CHINESE_FONT
    ((lv_font_t *)UI_FONT_CHINESE)->fallback =
        (const void *)&lv_font_montserrat_14;
#endif

    /* 创建主容器：水平 flex 布局，左侧对话窗口，右侧内存监控 */
    g_main_cont = lv_obj_create(scr);
    lv_obj_set_size(g_main_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g_main_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(g_main_cont, 0, 0);
    lv_obj_set_style_pad_column(g_main_cont, 0, 0);
    lv_obj_set_style_border_width(g_main_cont, 0, 0);
    lv_obj_set_style_bg_color(g_main_cont, lv_color_hex(UI_COLOR_BG), 0);

    /* ---- 7. 创建各窗口模块 ----
     * 主容器分为左右两区：
     *   左侧 (flex_grow=1)：对话窗口
     *   右侧 (固定 320px)：内存监控 */
    lv_obj_t *left = lv_obj_create(g_main_cont);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    /* 左侧与右侧之间加 1px 分隔线 */
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_border_color(left, lv_color_hex(UI_COLOR_BORDER), 0);

    /* ===== 二分排查：用 UI_TEST_MODULE 掏控制加载哪个模块 =====
     * 用法：编译时加 -DUI_TEST_MODULE=0 到 4
     *   0 = 全部禁用（只留基础屏幕+主容器）
     *   1 = 只加载 chat_window
     *   2 = 加载 chat_window + file_import
     *   3 = 加载 chat_window + file_import + mem_monitor
     *   4 = 全部加载（默认行为，等价于不定义 UI_TEST_MODULE）
     */
#ifndef UI_TEST_MODULE
#define UI_TEST_MODULE 4
#endif

#if UI_TEST_MODULE >= 2
    /* 文件导入面板：在主容器上创建（弹窗模式，初始隐藏） */
    if (ui_file_import_create(scr) != 0) {
        printf("[ui_main] 文件导入面板创建失败\n");
    }
    /* 知识库管理面板（阶段 6）：弹窗模式，初始隐藏，由聊天窗工具栏按钮打开 */
    if (ui_kb_manager_create(scr) != 0) {
        printf("[ui_main] 知识库管理面板创建失败\n");
    }
#endif

#if UI_TEST_MODULE >= 1
    /* 对话窗口：在左侧容器内 */
    if (ui_chat_window_create(left) != 0) {
        printf("[ui_main] 对话窗口创建失败\n");
    }
#endif

#if UI_TEST_MODULE >= 3
    /* 内存监控：直接放在主容器（自动成为右侧） */
    if (ui_mem_monitor_create(g_main_cont) != 0) {
        printf("[ui_main] 内存监控面板创建失败\n");
    }
#endif

#if UI_TEST_MODULE >= 4
    /* 悬浮窗：在屏幕上创建（floating 模式，独立位置） */
    if (ui_floating_window_create(scr) != 0) {
        printf("[ui_main] 悬浮小窗口创建失败\n");
    }
#endif

    /* ---- 8. 启动管道轮询 timer ---- */
    if (ui_pipeline_init() != 0) {
        printf("[ui_main] 管道桥接初始化失败\n");
    }

    printf("[ui_main] UI 初始化完成\n");
    return 0;
}


int ui_loop(void)
{
    /* 检查退出标志 */
    if (g_request_exit) {
        return -1;
    }

    /* ---- 1. 处理 SDL 事件 ----
     * sdl_process_events 返回 -1 表示收到退出事件 */
    if (sdl_process_events() != 0) {
        return -1;
    }

    /* ---- 2. LVGL 时基更新 ---- */
    lv_tick_inc(5);

    /* 录音中：轮询读取音频数据到缓冲区 */
    ui_main_poll_recording();

    /* ---- 3. LVGL 任务推进 ----
     * lv_timer_handler 处理所有 LVGL 内部任务：
     *   - 控件重绘
     *   - 动画推进
     *   - 用户注册的 lv_timer（含 ui_pipeline 轮询）
     * 必须周期性调用 */
    lv_timer_handler();

    /* ---- 4. 短暂休眠降低 CPU 占用 ---- */
    usleep(5000);

    return 0;
}


void ui_deinit(void)
{
    /* 释放显示缓冲区（单缓冲，仅 g_buf1） */
    if (g_buf1) { free(g_buf1); g_buf1 = NULL; }

    /* 释放 SDL 资源 */
    sdl_deinit();

    printf("[ui_main] UI 资源已释放\n");
}


/*
 * =============================================================================
 * 暴露给 ui_floating_window 的辅助接口
 * =============================================================================
 */

void ui_main_window_show(void)
{
    if (g_main_cont) {
        lv_obj_clear_flag(g_main_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_main_window_hide(void)
{
    if (g_main_cont) {
        lv_obj_add_flag(g_main_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

int ui_main_request_exit(void)
{
    /* 设置退出标志，ui_loop 下一轮检测到后返回 -1 */
    g_request_exit = 1;
    return 0;
}
