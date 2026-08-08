#ifndef ASR_ENGINE_H
#define ASR_ENGINE_H

/*
 * =============================================================================
 * EdgeSim ASR 引擎封装模块公共头文件  asr_engine.h
 * =============================================================================
 * 【文件作用】
 *   声明 asr_engine 模块的统一对外接口。
 *   本模块封装 whisper.cpp，加载 INT8 量化的 Whisper 模型，
 *   将音频文件（麦克风录音）转写为文字。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.4 节「AI引擎（统一接口）」
 *   EdgeSim_Design.md 第 1.2 节「Whisper离线语音交互」
 *
 * 【设计约束（严格遵守）】
 *   1. 仅封装 whisper.cpp 调用，不改造第三方库源码
 *   2. 不处理进程调度（归 multi_proc 模块）
 *   3. 不处理内存管理（归 hardware_sim 模块）
 *   4. 统一三接口：init / run / destroy
 *
 * 【条件编译】
 *   定义 HAS_WHISPER_CPP 时使用真实 whisper.cpp 库
 *   未定义时编译 mock 桩（仅测试接口连通性）
 *
 * 【ASR 推理流程（背景知识，新手必读）】
 *   1. 用户传入音频文件路径（必须是 16kHz 单声道 16-bit WAV）
 *   2. 引擎检查并读取 WAV（格式不符直接报错，不做重采样）
 *   3. 调用 whisper.cpp 推理：编码器 + 解码器
 *   4. 输出转写文本（支持多语言自动检测）
 *   5. 返回文字字符串
 *
 * 【适用场景】
 *   - 语音提问：用户口述问题，转文字后送给 LLM
 *   - 语音备忘录：录音转文字保存到 SQLite
 *   - 语音翻译：转文字后送给 LLM 翻译
 * =============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * asr_engine_init —— 加载 Whisper 语音模型
 * 【参数】model_path：模型文件路径（如 "ggml-base-int8.bin"）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部完成：whisper 初始化 → 模型加载 → 推理上下文创建
 */
int asr_engine_init(const char *model_path);

/*
 * asr_engine_run —— 执行一次语音转文字
 * 【参数】input：音频文件路径（推荐 16kHz 单声道 WAV）
 * 【参数】output：输出缓冲区，存放转写文字
 *               调用者需分配至少 8192 字节（ENGINE_OUTPUT_MAX）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部完成：音频读取 → 重采样 → whisper 推理 → 取回文本
 */
int asr_engine_run(const char *input, char *output);

/*
 * asr_engine_destroy —— 销毁引擎，释放资源
 * 【说明】释放 whisper 上下文与相关缓冲区
 */
void asr_engine_destroy(void);


#ifdef __cplusplus
}
#endif

#endif /* ASR_ENGINE_H */
