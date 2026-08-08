#ifndef LLM_ENGINE_H
#define LLM_ENGINE_H

/*
 * =============================================================================
 * EdgeSim LLM 引擎封装模块公共头文件  llm_engine.h
 * =============================================================================
 * 【文件作用】
 *   声明 llm_engine 模块的统一对外接口。
 *   本模块封装 llama.cpp，加载 INT8 量化的 Qwen1.8B 模型，提供离线对话推理。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.4 节「AI引擎（统一接口）」
 *
 * 【设计约束（严格遵守）】
 *   1. 仅封装 llama.cpp 调用，不改造第三方库源码
 *   2. 不处理进程调度（归 multi_proc 模块）
 *   3. 不处理内存管理（归 hardware_sim 模块）
 *   4. 统一三接口：init / run / destroy
 *
 * 【条件编译】
 *   定义 HAS_LLAMA_CPP 时使用真实 llama.cpp 库
 *   未定义时编译 mock 桩（不依赖库，仅测试接口连通性）
 * =============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * llm_engine_init —— 加载 LLM 模型
 * 【参数】model_path：模型文件路径（如 "qwen1.8b-int8.gguf"）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部完成：backend初始化 → 模型加载 → 推理上下文创建
 */
int llm_engine_init(const char *model_path);

/*
 * llm_engine_run —— 执行一轮对话推理
 * 【参数】input：用户输入文本（问题/指令）
 * 【参数】output：输出缓冲区，存放 AI 回复文本
 *               调用者需分配至少 8192 字节（ENGINE_OUTPUT_MAX）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部完成：分词 → 预填充(prefill) → 逐token采样 → 拼接回复
 */
int llm_engine_run(const char *input, char *output);

/*
 * llm_engine_destroy —— 销毁引擎，释放资源
 * 【说明】释放推理上下文、模型、后端资源
 */
void llm_engine_destroy(void);


#ifdef __cplusplus
}
#endif

#endif /* LLM_ENGINE_H */
