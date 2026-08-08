#ifndef OCR_ENGINE_H
#define OCR_ENGINE_H

/*
 * =============================================================================
 * EdgeSim OCR 引擎封装模块公共头文件  ocr_engine.h
 * =============================================================================
 * 【文件作用】
 *   声明 ocr_engine 模块的统一对外接口。
 *   本模块封装 RKNN Lite（Rockchip NPU）或 ONNX Runtime（PC 端）两种后端，
 *   加载 OCR 模型（如 PaddleOCR 文字检测 + 识别），将图片转为文字。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.4 节「AI引擎（统一接口）」
 *   EdgeSim_Design.md 第 1.2 节「RKNN轻量化OCR文字识别」
 *
 * 【设计约束（严格遵守）】
 *   1. 仅封装 RKNN Lite 调用，不改造第三方库源码
 *   2. 不处理进程调度（归 multi_proc 模块）
 *   3. 不处理内存管理（归 hardware_sim 模块）
 *   4. 统一三接口：init / run / destroy
 *
 * 【条件编译】
 *   定义 HAS_RKNN_LITE    时使用真实 RKNN Lite 库（仅 Rockchip 平台可链接）
 *   定义 HAS_ONNXRUNTIME  时使用 ONNX Runtime 推理（PC 端加载 .onnx 模型）
 *   两者均未定义时编译 mock 桩（仅测试接口连通性）
 *
 * 【OCR 推理流程（背景知识，新手必读）】
 *   1. 用户传入图片路径（截图 / 照片 / 手写笔记扫描图）
 *   2. 引擎读取图片 → 预处理（缩放到模型输入尺寸，如 320x320）
 *   3. 调用 RKNN 模型推理（DBNet 文字检测 + CRNN 文字识别）
 *   4. 后处理：将模型输出张量解码为可读文本
 *   5. 返回识别到的文字字符串
 * =============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ocr_engine_init —— 加载 OCR 模型
 * 【参数】model_path：RKNN 模型文件路径（如 "paddleocr-int8.rknn"）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部完成：RKNN 运行时初始化 → 模型加载 → 输入/输出属性查询
 * 【注意】RKNN 模型必须与目标 NPU 平台匹配（RK3566/RK3568/RV1106 不通用）
 */
int ocr_engine_init(const char *model_path);

/*
 * ocr_engine_run —— 执行一次图片文字识别
 * 【参数】input：图片文件路径（支持 jpg/png/bmp）
 * 【参数】output：输出缓冲区，存放识别到的文字
 *               调用者需分配至少 8192 字节（ENGINE_OUTPUT_MAX）
 * 【返回值】0=成功，-1=失败
 * 【说明】内部完成：图片读取 → 预处理 → RKNN 推理 → 后处理解码
 *               多行文字以 '\n' 分隔
 */
int ocr_engine_run(const char *input, char *output);

/*
 * ocr_engine_destroy —— 销毁引擎，释放资源
 * 【说明】释放 RKNN 上下文与相关缓冲区
 */
void ocr_engine_destroy(void);


#ifdef __cplusplus
}
#endif

#endif /* OCR_ENGINE_H */
