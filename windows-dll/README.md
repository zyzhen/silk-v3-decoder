# windows-dll —— SILK v3 编解码器 Windows 动态库（silkcodec.dll）

把本仓库 `silk/` 目录下的 Skype SILK v3 定点编解码核心（纯 C、无第三方依赖）
封装为 Windows 动态库，供 C/C++、C#、Python 等任意语言直接调用，
不再需要以子进程方式调用 `silk_v3_decoder.exe` / `silk_v3_encoder.exe`。

支持解码/编码微信 `amr`、`aud` 文件与 QQ `slk` 文件（自动识别 `0x02` 腾讯前缀）。

> 本库只做 **SILK ↔ PCM**。PCM 转 mp3 等其他格式请自行处理
>（例如继续用 `windows/` 目录下的 `lame.exe` 或 ffmpeg）。

## 目录结构

```
windows-dll
  ├── silk_dll.h        导出 API 头文件（中文注释，含完整参数说明）
  ├── silk_dll.c        实现（解码/编码主循环移植自 silk/test 下的测试程序）
  ├── silk_dll.def      模块定义文件（追加导出底层 SKP_Silk_SDK_* 原生函数）
  ├── silk_dll.rc       DLL 版本资源
  ├── build.ps1         一键构建脚本（MSVC，自动定位最新 Visual Studio）
  ├── CMakeLists.txt    CMake 构建（可选，供跨环境使用）
  └── test/test_dll.c   端到端自测程序（LoadLibrary 动态加载验证）
```

## 构建

要求：安装带 C++ 工作负载的 Visual Studio（或 Build Tools）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1            # x64 + x86
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Arch x64  # 仅 x64
```

产物在 `build\x64\`、`build\x86\` 下：

- `silkcodec.dll` —— 动态库（静态 CRT `/MT`，目标机器无需装 VC 运行库）
- `silkcodec.lib` —— 导入库（静态链接调用时使用）
- `test_dll.exe` —— 自测程序，构建脚本默认会自动运行

也可用 CMake：

```powershell
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
```

## 导出 API 一览

高层便捷 API（声明与详细说明见 [silk_dll.h](silk_dll.h)，全部 cdecl 调用约定）：

| 函数 | 功能 |
| --- | --- |
| `silk_decode_buffer` | 内存中 SILK 码流 → 16bit 单声道 PCM |
| `silk_encode_buffer` | 内存中 PCM → SILK 码流（可选 QQ/微信兼容格式） |
| `silk_decode_file` / `silk_decode_file_w` | SILK 文件 → 原始 PCM 文件（s16le） |
| `silk_encode_file` / `silk_encode_file_w` | 原始 PCM 文件 → SILK 文件 |
| `silk_free` | 释放库分配并返回的缓冲区 |
| `silk_dll_version` | 返回 DLL 版本字符串 |

底层 Skype SILK SDK 原生 API（逐包流式处理时使用，声明见
[../silk/interface/SKP_Silk_SDK_API.h](../silk/interface/SKP_Silk_SDK_API.h)）：
`SKP_Silk_SDK_Get_Encoder_Size`、`SKP_Silk_SDK_InitEncoder`、`SKP_Silk_SDK_QueryEncoder`、
`SKP_Silk_SDK_Encode`、`SKP_Silk_SDK_Get_Decoder_Size`、`SKP_Silk_SDK_InitDecoder`、
`SKP_Silk_SDK_Decode`、`SKP_Silk_SDK_search_for_LBRR`、`SKP_Silk_SDK_get_TOC`、
`SKP_Silk_SDK_get_version`。

## 调用示例

### C / C++

```c
#include "silk_dll.h"   /* 链接 silkcodec.lib，或自行 LoadLibrary */

/* 微信语音文件 -> PCM（24kHz s16le 单声道） */
short* pcm = NULL;
int    samples = 0;
int    ret = silk_decode_buffer(silk_bytes, silk_size, 24000, &pcm, &samples);
if (ret == SILK_DLL_OK) {
    /* 使用 pcm[0..samples-1] ... */
    silk_free(pcm);
}

/* PCM -> 微信兼容 silk（0x02 前缀） */
unsigned char* silk = NULL;
int silk_size2 = 0;
ret = silk_encode_buffer(pcm_data, pcm_samples, 24000, 25000, /*tencent=*/1,
                         &silk, &silk_size2);
```

### C#（P/Invoke）

```csharp
using System.Runtime.InteropServices;

static class SilkCodec
{
    [DllImport("silkcodec.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int silk_decode_file(string silkPath, string pcmPath, int sampleRate);

    [DllImport("silkcodec.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int silk_encode_file(string pcmPath, string silkPath,
                                              int sampleRate, int bitRate, int tencent);
}

// 微信语音 -> PCM 文件
int ret = SilkCodec.silk_decode_file("msg.amr", "msg.pcm", 24000);
```

### Python（ctypes）

```python
import ctypes

silk = ctypes.CDLL(r"silkcodec.dll")

# 微信语音 -> PCM 文件
ret = silk.silk_decode_file(b"msg.amr", b"msg.pcm", 24000)
assert ret == 0

# 内存级解码
data = open("msg.amr", "rb").read()
pcm_ptr = ctypes.POINTER(ctypes.c_short)()
n = ctypes.c_int(0)
ret = silk.silk_decode_buffer(data, len(data), 24000,
                              ctypes.byref(pcm_ptr), ctypes.byref(n))
assert ret == 0
pcm_bytes = ctypes.string_at(pcm_ptr, n.value * 2)   # s16le 裸 PCM
silk.silk_free(pcm_ptr)
```

得到 PCM 后转 mp3 示例（ffmpeg）：

```
ffmpeg -y -f s16le -ar 24000 -ac 1 -i msg.pcm msg.mp3
```

## 兼容性验证

已在本仓库自带的原版命令行工具上做过交叉验证（同一输入）：

- 本 DLL **编码**输出与 `windows/silk_v3_encoder.exe -tencent` 输出**逐字节一致**；
- 本 DLL **解码**输出与 `windows/silk_v3_decoder.exe` 输出**逐字节一致**；
- 双向互解：DLL 编码的文件原版 exe 可解，exe 编码的文件本 DLL 可解。

## 错误码

| 值 | 含义 |
| --- | --- |
| `0`  | 成功 |
| `-1` | 参数非法（空指针 / 采样率不受支持 / 输入不足 20ms 等） |
| `-2` | 输入不是合法的 SILK v3 码流 |
| `-3` | 内存分配失败 |
| `-4` | 解码失败（没有解出任何有效数据包） |
| `-5` | 编码失败 |
| `-6` | 文件读写失败 |

编码采样率仅支持 `8000/12000/16000/24000/32000/44100/48000`（SILK 编码器硬性要求）；
解码输出采样率支持 `[8000, 48000]` 任意值（内部自动重采样）。

## 许可

SILK SDK 版权归 Skype Limited（见各源文件头部声明）；封装层与本仓库其余部分同为 MIT 许可。
