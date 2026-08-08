/*
 * =============================================================================
 * EdgeSim LLM 引擎封装模块实现文件  llm_engine.c
 * =============================================================================
 * 【文件作用】
 *   封装 llama.cpp C API，实现统一的 init/run/destroy 三接口。
 *   加载 INT8 量化 Qwen1.8B 模型，提供离线文本生成推理。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.4 节
 *
 * 【llama.cpp API 核心流程（新手重点）】
 *   1. llama_backend_init()                    初始化后端（进程级，调一次）
 *   2. llama_model_load_from_file(path, params) 加载 GGUF 模型文件
 *   3. llama_new_context_with_model(model, ctx_params)  创建推理上下文
 *   4. llama_tokenize(model, text, tokens, ...)         文本→token序列
 *   5. llama_decode(ctx, batch)                          执行推理（前向传播）
 *   6. llama_sampler_sample(sampler, ctx, idx)           采样下一个 token
 *   7. llama_token_to_piece(ctx, token, buf, size)       token→文本
 *   8. llama_free(ctx) / llama_model_free(model)         释放资源
 *   9. llama_backend_free()                              释放后端
 *
 * 【条件编译】
 *   HAS_LLAMA_CPP 定义时：调用真实 llama.cpp API
 *   未定义时：运行 mock 桩，返回模拟结果（便于无库环境测试接口）
 * =============================================================================
 */

#include "llm_engine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 输出缓冲区最大长度 */
#define ENGINE_OUTPUT_MAX 8192

/* =========================================================================
 * 条件编译：真实 llama.cpp 实现
 * ========================================================================= */
#ifdef HAS_LLAMA_CPP

#include "llama.h"

/* ---- 模块内部状态（static，仅本文件可见）---- */
static struct llama_model    *g_model      = NULL;  /* 模型句柄 */
static struct llama_context  *g_context    = NULL;  /* 推理上下文 */
static struct llama_sampler  *g_sampler    = NULL;  /* 采样器 */
static const struct llama_vocab *g_vocab   = NULL;  /* 词表句柄（新版 API 需要） */
static int                    g_initialized = 0;     /* 初始化标志 */

/*
 * 接口 1：llm_engine_init —— 加载模型
 */
int llm_engine_init(const char *model_path)
{
    struct llama_model_params   model_params;  /* 模型加载参数 */
    struct llama_context_params ctx_params;    /* 上下文参数 */

    if (model_path == NULL) {
        return -1;
    }
    if (g_initialized) {
        printf("[llm_engine] 已初始化，请先 destroy\n");
        return -1;
    }

    /* ---- 1. 初始化后端 ----
     * llama_backend_init 在进程内初始化 llama.cpp 运行时。
     * 只需调用一次，重复调用无副作用但浪费。 */
    llama_backend_init();

    /* ---- 2. 配置模型加载参数 ----
     * llama_model_default_params 返回默认参数结构体。
     * n_gpu_layers=0 表示纯 CPU 推理（嵌入式设备无 GPU） */
    model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0;   /* CPU only，适配低端设备 */

    /* ---- 3. 加载模型文件 ----
     * llama_model_load_from_file 读取 GGUF 格式模型文件。
     * 返回 NULL 表示加载失败（路径错误/文件损坏/内存不足）。 */
    g_model = llama_model_load_from_file(model_path, model_params);
    if (g_model == NULL) {
        printf("[llm_engine] 模型加载失败: %s\n", model_path);
        llama_backend_free();
        return -1;
    }
    printf("[llm_engine] 模型加载成功: %s\n", model_path);

    /* 获取词表句柄（新版 API 的 tokenize/token_to_piece/is_eog 都需要 vocab） */
    g_vocab = llama_model_get_vocab(g_model);

    /* ---- 4. 配置推理上下文参数 ----
     * n_ctx=2048：上下文窗口大小（token 数），越大越占内存
     * n_batch=512：单次前向传播最大 token 数
     * n_threads=4：CPU 推理线程数 */
    ctx_params = llama_context_default_params();
    ctx_params.n_ctx     = 2048;
    ctx_params.n_batch   = 512;
    ctx_params.n_threads = 4;
    ctx_params.n_threads_batch = 4;

    /* ---- 5. 创建推理上下文 ---- */
    g_context = llama_new_context_with_model(g_model, ctx_params);
    if (g_context == NULL) {
        printf("[llm_engine] 创建上下文失败\n");
        llama_model_free(g_model);
        g_model = NULL;
        llama_backend_free();
        return -1;
    }

    /* ---- 6. 创建采样器 ----
     * 采样器决定如何从模型输出的概率分布中选取下一个 token。
     * 链式采样：top_k → top_p → temperature → dist
     *   top_k(40)   ：只从概率最高的 40 个 token 中选
     *   top_p(0.9)  ：累积概率达 0.9 时截断（nucleus sampling）
     *   temp(0.8)   ：温度，越高越随机，越低越确定
     *   dist(0)     ：按概率随机采样（0=始终采样） */
    struct llama_sampler_chain_params sampler_params = llama_sampler_chain_default_params();
    g_sampler = llama_sampler_chain_init(sampler_params);
    llama_sampler_chain_add(g_sampler, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(g_sampler, llama_sampler_init_top_p(0.9, 1));
    llama_sampler_chain_add(g_sampler, llama_sampler_init_temp(0.8));
    llama_sampler_chain_add(g_sampler, llama_sampler_init_dist(0));

    g_initialized = 1;
    printf("[llm_engine] 推理上下文创建成功，n_ctx=2048\n");
    return 0;
}

/*
 * 接口 2：llm_engine_run —— 执行对话推理
 */
int llm_engine_run(const char *input, char *output)
{
    /* token 数组，存放输入文本的分词结果 */
    llama_token tokens[1024];
    int n_tokens;          /* 实际分词数 */
    int i;                 /* 循环变量 */
    int output_len = 0;    /* 输出文本当前长度 */
    int max_gen_tokens = 64;  /* 最多生成 64 个 token（简单问题简短回答） */

    /* Qwen2.5-Instruct 对话模板：
     * <|im_start|>system\n{系统提示}<|im_end|>\n
     * <|im_start|>user\n{用户输入}<|im_end|>\n
     * <|im_start|>assistant\n
     * 不用此模板模型会输出乱码 */
    char prompt[2048];
    /* 系统提示用简体中文（阶段 4.5）：Qwen2.5 原生支持中文，
     * 中文 system 提示引导模型用中文回答（配合 UI 中文字体显示）。 */
    snprintf(prompt, sizeof(prompt),
             "<|im_start|>system\n你是 EdgeSim 的 AI 助手，请用简体中文简洁回答，一两句话即可。<|im_end|>\n"
             "<|im_start|>user\n%s<|im_end|>\n"
             "<|im_start|>assistant\n",
             input);

    if (!g_initialized || input == NULL || output == NULL) {
        return -1;
    }

    /* ---- 1. 分词：将对话模板 prompt 转为 token 序列 ----
     * add_special=false：不加 BOS（对话模板自带特殊 token）
     * parse_special=true：解析 <|im_start|> 等特殊 token */
    n_tokens = llama_tokenize(g_vocab, prompt, strlen(prompt), tokens, 1024, false, true);
    if (n_tokens < 0) {
        printf("[llm_engine] 分词失败\n");
        return -1;
    }
    printf("[llm_engine] 输入分词完成，%d 个 token\n", n_tokens);

    /* ---- 2. 预填充(prefill)：将输入 token 全部送入模型解码 ----
     * 创建 batch，放入所有输入 token，一次 decode 完成前向传播。 */
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);

    /* llama_decode 执行前向传播，计算所有位置的概率分布。
     * 返回 0=成功，<0=错误（如超出上下文长度） */
    if (llama_decode(g_context, batch) != 0) {
        printf("[llm_engine] prefill decode 失败\n");
        return -1;
    }

    /* ---- 3. 逐 token 生成回复 ---- */
    output[0] = '\0';
    for (i = 0; i < max_gen_tokens; i++) {
        /* 从最后一个位置的 logits 中采样下一个 token。
         * llama_sampler_sample 参数：
         *   sampler ：采样器链
         *   ctx     ：推理上下文
         *   -1      ：采样最后一个位置（-1 表示最后） */
        llama_token new_token = llama_sampler_sample(g_sampler, g_context, -1);

        /* 检查是否生成了 EOS(End Of Sequence) token，表示回复结束。
         * 新版 API 用 llama_vocab_is_eog 替代 llama_token_is_eog */
        if (llama_vocab_is_eog(g_vocab, new_token)) {
            printf("[llm_engine] 生成结束（EOS），共 %d 个 token\n", i);
            break;
        }

        /* 将 token 转为文本片段并拼接到输出。
         * llama_token_to_piece 参数（新版 API 用 vocab 替代 ctx，多了 lstrip 和 special 参数）：
         *   vocab     ：词表句柄
         *   new_token ：要转换的 token
         *   buf       ：输出缓冲区
         *   length    ：缓冲区大小
         *   lstrip    ：左侧空格剥离（0=不剥离）
         *   special   ：是否解析特殊 token */
        char piece[32];
        int piece_len = llama_token_to_piece(g_vocab, new_token, piece, sizeof(piece), 0, false);
        if (piece_len > 0 && output_len + piece_len < ENGINE_OUTPUT_MAX - 1) {
            /* llama_token_to_piece 不保证 null 终止，必须手动添加
             * 否则 strcat 会读取缓冲区残留数据导致乱码 */
            piece[piece_len] = '\0';
            strcat(output, piece);
            output_len += piece_len;
        }

        /* 将新 token 送入模型，继续生成下一个。
         * llama_batch_get_one 创建只含 1 个 token 的 batch。 */
        batch = llama_batch_get_one(&new_token, 1);
        if (llama_decode(g_context, batch) != 0) {
            printf("[llm_engine] 生成 decode 失败，提前结束\n");
            break;
        }
    }

    printf("[llm_engine] 推理完成，输出 %d 字节\n", output_len);
    return 0;
}

/*
 * 接口 3：llm_engine_destroy —— 销毁引擎
 */
void llm_engine_destroy(void)
{
    if (!g_initialized) {
        return;
    }

    /* 按创建的逆序释放：采样器 → 上下文 → 模型 → 后端 */
    if (g_sampler) {
        llama_sampler_free(g_sampler);
        g_sampler = NULL;
    }
    if (g_context) {
        llama_free(g_context);
        g_context = NULL;
    }
    if (g_model) {
        llama_model_free(g_model);
        g_model = NULL;
    }
    llama_backend_free();

    g_initialized = 0;
    printf("[llm_engine] 资源已释放\n");
}


/* =========================================================================
 * 条件编译：Mock 桩实现（无 llama.cpp 时编译）
 * ========================================================================= */
#else  /* !HAS_LLAMA_CPP */

static int g_mock_initialized = 0;

int llm_engine_init(const char *model_path)
{
    if (model_path == NULL) {
        return -1;
    }
    if (g_mock_initialized) {
        return -1;
    }
    /* Mock 模式：不真正加载模型，仅打印提示 */
    printf("[llm_engine MOCK] 模拟加载模型: %s\n", model_path);
    printf("[llm_engine MOCK] (未定义 HAS_LLAMA_CPP，使用 mock 桩)\n");
    g_mock_initialized = 1;
    return 0;
}

int llm_engine_run(const char *input, char *output)
{
    if (!g_mock_initialized || input == NULL || output == NULL) {
        return -1;
    }
    /* Mock 模式：返回模拟回复，验证接口连通性 */
    printf("[llm_engine MOCK] 输入: %s\n", input);
    snprintf(output, ENGINE_OUTPUT_MAX,
             "[MOCK LLM] You said: \"%s\".\n"
             "Hint: Define HAS_LLAMA_CPP and link llama.cpp for real inference.",
             input);
    printf("[llm_engine MOCK] 已生成模拟回复\n");
    return 0;
}

void llm_engine_destroy(void)
{
    if (!g_mock_initialized) {
        return;
    }
    printf("[llm_engine MOCK] 模拟释放资源\n");
    g_mock_initialized = 0;
}

#endif /* HAS_LLAMA_CPP */
