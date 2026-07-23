/***********************************************************************
 * silk_dll.h —— SILK v3 音频编解码器 Windows 动态库（silkcodec.dll）导出接口
 *
 * 本动态库把仓库 silk/ 目录下的 Skype SILK v3 定点编解码核心（纯 C，
 * 无任何第三方依赖）封装为 Windows DLL，提供两层 API：
 *
 *   1. 高层便捷 API（本头文件声明，silk_dll.c 实现）：
 *      - 内存级：silk_decode_buffer / silk_encode_buffer
 *        直接在内存中完成 SILK v3 码流 <-> 16bit PCM 的互转，
 *        兼容微信 amr/aud、QQ slk 等文件（自动识别 0x02 腾讯前缀）。
 *      - 文件级：silk_decode_file / silk_encode_file（另有 *_w 宽字符版），
 *        行为对应仓库自带命令行工具 silk_v3_decoder / silk_v3_encoder。
 *
 *   2. 底层原生 SDK API（见 silk/interface/SKP_Silk_SDK_API.h）：
 *      SKP_Silk_SDK_* 系列函数按原样通过 silk_dll.def 一并导出，
 *      需要逐包流式处理的调用方可以直接使用。
 *
 * 注意：本库只做 SILK <-> PCM。PCM 转 mp3 等其他格式请调用方
 * 自行处理（例如继续用仓库 windows/ 目录下的 lame.exe / ffmpeg）。
 *
 * 所有导出函数均为 C 调用约定（cdecl），线程安全性：不同线程操作
 * 各自独立的转换调用互不影响（库内部无全局可变状态）。
 ***********************************************************************/

#ifndef SILK_DLL_H
#define SILK_DLL_H

#include <wchar.h> /* wchar_t（宽字符文件路径 API 使用） */

/* 导出/导入宏：编译 DLL 本体时由构建系统定义 SILK_DLL_EXPORTS */
#if defined(_WIN32)
#  if defined(SILK_DLL_EXPORTS)
#    define SILK_DLL_API __declspec(dllexport)
#  elif defined(SILK_DLL_STATIC)
#    define SILK_DLL_API
#  else
#    define SILK_DLL_API __declspec(dllimport)
#  endif
#else
#  define SILK_DLL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*======================= 错误码定义 =======================*/
/* 所有返回 int 的接口：0 表示成功，负值表示失败 */
#define SILK_DLL_OK               0   /* 成功                                     */
#define SILK_DLL_ERR_INVALID_ARG -1   /* 参数非法（空指针/长度/采样率越界等）     */
#define SILK_DLL_ERR_BAD_HEADER  -2   /* 输入不是合法的 SILK v3 码流（头部不符） */
#define SILK_DLL_ERR_ALLOC       -3   /* 内存分配失败                             */
#define SILK_DLL_ERR_DECODE      -4   /* 解码失败（没有解出任何有效数据包）       */
#define SILK_DLL_ERR_ENCODE      -5   /* 编码失败（SDK 返回错误）                 */
#define SILK_DLL_ERR_IO          -6   /* 文件读写失败                             */

/*======================= 版本信息 =======================*/

/* 返回本 DLL 的版本字符串（静态存储，勿释放），例如 "1.0.0"。
 * 底层 SILK SDK 的版本可另行调用导出的 SKP_Silk_SDK_get_version()。 */
SILK_DLL_API const char* silk_dll_version(void);

/*======================= 内存级 API =======================*/

/* 解码：SILK v3 码流 -> 16bit 小端单声道 PCM
 *
 * 输入的码流应为完整的 SILK v3 文件内容，支持两种头部：
 *   - 标准头       "#!SILK_V3"          （9 字节）
 *   - 腾讯变体头   0x02 + "#!SILK_V3"   （10 字节，微信 amr/aud、QQ slk）
 *
 * 参数：
 *   silk_data    [in]  SILK 码流首地址
 *   silk_size    [in]  码流字节数
 *   sample_rate  [in]  期望的输出 PCM 采样率（Hz）。传 0 使用默认 24000。
 *                      有效范围 [8000, 48000]，非 SILK 内部采样率时自动重采样。
 *   pcm_out      [out] *pcm_out 接收解码得到的 PCM 缓冲区（库内 malloc 分配，
 *                      使用完毕必须调用 silk_free() 释放）
 *   pcm_samples  [out] *pcm_samples 接收样本个数（每样本 2 字节）
 *
 * 返回：SILK_DLL_OK 或负值错误码。失败时 *pcm_out / *pcm_samples 不可用。 */
SILK_DLL_API int silk_decode_buffer(
    const unsigned char* silk_data,
    int                  silk_size,
    int                  sample_rate,
    short**              pcm_out,
    int*                 pcm_samples);

/* 编码：16bit 小端单声道 PCM -> SILK v3 码流
 *
 * 参数：
 *   pcm          [in]  PCM 样本首地址（16bit 有符号、小端、单声道）
 *   pcm_samples  [in]  样本个数
 *   sample_rate  [in]  PCM 采样率（Hz）。传 0 使用默认 24000。
 *                      必须是 8000/12000/16000/24000/32000/44100/48000 之一
 *                      （SILK 编码器的硬性要求）。
 *   bit_rate     [in]  目标码率（bps）。传 0 使用默认 25000。
 *   tencent      [in]  非 0 时生成 QQ/微信兼容格式：头部带 0x02 前缀、
 *                      结尾不写 -1 终止标记；0 时生成标准 SILK v3 文件。
 *   silk_out     [out] *silk_out 接收编码得到的码流缓冲（库内 malloc 分配，
 *                      使用完毕必须调用 silk_free() 释放）
 *   silk_size    [out] *silk_size 接收码流字节数
 *
 * 说明：编码按 20ms 一帧进行，末尾不足 20ms 的样本会被丢弃
 *（与仓库自带 silk_v3_encoder 行为一致）。输入不足一帧时返回
 * SILK_DLL_ERR_INVALID_ARG。
 *
 * 返回：SILK_DLL_OK 或负值错误码。 */
SILK_DLL_API int silk_encode_buffer(
    const short*         pcm,
    int                  pcm_samples,
    int                  sample_rate,
    int                  bit_rate,
    int                  tencent,
    unsigned char**      silk_out,
    int*                 silk_size);

/* 释放本库分配并通过出参返回的缓冲区（pcm_out / silk_out）。
 * 传入 NULL 安全无操作。 */
SILK_DLL_API void silk_free(void* ptr);

/*======================= 文件级 API =======================*/

/* 解码文件：SILK v3 文件 -> 原始 PCM 文件（s16le 裸数据，无 WAV 头）
 * sample_rate 含义同 silk_decode_buffer。
 * 对应命令行用法：silk_v3_decoder.exe in.silk out.pcm */
SILK_DLL_API int silk_decode_file(
    const char* silk_path,
    const char* pcm_path,
    int         sample_rate);

/* 编码文件：原始 PCM 文件（s16le）-> SILK v3 文件
 * 参数含义同 silk_encode_buffer。
 * 对应命令行用法：silk_v3_encoder.exe in.pcm out.silk [-tencent] */
SILK_DLL_API int silk_encode_file(
    const char* pcm_path,
    const char* silk_path,
    int         sample_rate,
    int         bit_rate,
    int         tencent);

/* 宽字符（UTF-16）路径版本：Windows 下处理中文等非 ANSI 路径时使用 */
SILK_DLL_API int silk_decode_file_w(
    const wchar_t* silk_path,
    const wchar_t* pcm_path,
    int            sample_rate);

SILK_DLL_API int silk_encode_file_w(
    const wchar_t* pcm_path,
    const wchar_t* silk_path,
    int            sample_rate,
    int            bit_rate,
    int            tencent);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SILK_DLL_H */
