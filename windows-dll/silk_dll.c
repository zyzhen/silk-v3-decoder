/***********************************************************************
 * silk_dll.c —— SILK v3 编解码器 Windows 动态库实现
 *
 * 解码/编码主循环忠实移植自仓库 silk/test/Decoder.c 与 Encoder.c
 * （kn007 修改版，版本 20160922），去掉了其中的丢包模拟、FEC 搜索、
 * 计时统计等仅用于命令行测试的逻辑，把文件读写改为内存缓冲操作：
 *
 *   - 头部识别：兼容标准头 "#!SILK_V3" 与腾讯变体 0x02+"#!SILK_V3"
 *     （微信 amr/aud、QQ slk。0x02 前缀即 Encoder.c 中 -tencent 选项
 *     写入的原始字节）。
 *   - 码流布局：头部之后是若干 [int16 长度(小端)][负载] 数据包，
 *     标准文件以长度 -1 作为结束标记，腾讯变体无结束标记。
 *   - 每包可含最多 5 个 20ms 内部帧，循环解码直到
 *     moreInternalDecoderFrames 为 0（同 Decoder.c）。
 *   - 长度为 0 的包（DTX 静默期占位包）按丢包处理，调用解码器的
 *     PLC 丢包隐藏生成对应时长的输出（同 Decoder.c 的 lost 分支）。
 ***********************************************************************/

#ifdef _WIN32
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SKP_Silk_SDK_API.h"
#include "silk_dll.h"

/*======================= 常量（与 silk/test 下测试程序保持一致） =======================*/

#define SILK_HEADER            "#!SILK_V3" /* SILK v3 文件魔数            */
#define SILK_HEADER_LEN        9           /* 魔数长度                     */
#define TENCENT_PREFIX         0x02        /* QQ/微信变体的前缀字节        */

#define DEC_MAX_BYTES_PER_FRAME 1024       /* 解码：单帧最大字节数（Decoder.c） */
#define ENC_MAX_BYTES_PER_FRAME 250        /* 编码：单帧最大字节数，约 100kbps 峰值（Encoder.c） */
#define MAX_INPUT_FRAMES        5          /* 每包最多帧数                 */
#define FRAME_LENGTH_MS         20         /* 帧长（毫秒）                 */
#define MAX_API_FS_KHZ          48         /* API 支持的最高采样率（kHz）  */

#define DEFAULT_SAMPLE_RATE     24000      /* 默认采样率（converter.sh 同款） */
#define DEFAULT_BIT_RATE        25000      /* 默认编码码率（Encoder.c 同款）  */

/* 解码单包的最大输出样本数：(20ms * 48kHz * 2) * 5 帧，同 Decoder.c 的 out[] 尺寸 */
#define DEC_FRAME_BUF_SAMPLES  ( ( ( FRAME_LENGTH_MS * MAX_API_FS_KHZ ) << 1 ) * MAX_INPUT_FRAMES )

/*======================= 内部工具 =======================*/

/* 可增长的字节缓冲区（编码输出用） */
typedef struct {
    unsigned char* data;
    int            size;  /* 已写入字节数 */
    int            cap;   /* 已分配容量   */
} byte_buf_t;

/* 可增长的样本缓冲区（解码输出用） */
typedef struct {
    short* data;
    int    size;  /* 已写入样本数 */
    int    cap;   /* 已分配容量   */
} pcm_buf_t;

/* 向字节缓冲追加数据；容量不足时按 2 倍扩容。返回 0 成功，-1 分配失败 */
static int byte_buf_append( byte_buf_t* b, const void* src, int len )
{
    if( b->size + len > b->cap ) {
        int new_cap = b->cap > 0 ? b->cap : 4096;
        unsigned char* p;
        while( new_cap < b->size + len ) {
            new_cap *= 2;
        }
        p = (unsigned char*)realloc( b->data, (size_t)new_cap );
        if( p == NULL ) {
            return -1;
        }
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy( b->data + b->size, src, (size_t)len );
    b->size += len;
    return 0;
}

/* 向样本缓冲追加样本；容量不足时按 2 倍扩容。返回 0 成功，-1 分配失败 */
static int pcm_buf_append( pcm_buf_t* b, const short* src, int samples )
{
    if( b->size + samples > b->cap ) {
        int new_cap = b->cap > 0 ? b->cap : 24000;
        short* p;
        while( new_cap < b->size + samples ) {
            new_cap *= 2;
        }
        p = (short*)realloc( b->data, (size_t)new_cap * sizeof( short ) );
        if( p == NULL ) {
            return -1;
        }
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy( b->data + b->size, src, (size_t)samples * sizeof( short ) );
    b->size += samples;
    return 0;
}

/*======================= 版本 =======================*/

const char* silk_dll_version( void )
{
    return "1.0.0";
}

void silk_free( void* ptr )
{
    /* 与库内 malloc/realloc 配对；free(NULL) 本身安全 */
    free( ptr );
}

/*======================= 解码 =======================*/

int silk_decode_buffer(
    const unsigned char* silk_data,
    int                  silk_size,
    int                  sample_rate,
    short**              pcm_out,
    int*                 pcm_samples )
{
    SKP_SILK_SDK_DecControlStruct dec_ctrl;
    void*      dec_state = NULL;
    SKP_int32  dec_size  = 0;
    pcm_buf_t  out       = { NULL, 0, 0 };
    int        pos       = 0;
    int        packets   = 0;
    int        ret;
    /* 单包解码暂存区约 19KB，放堆上以免占用调用方线程过多栈空间 */
    short*     frame_buf = NULL;
    /* PLC 丢包隐藏分支给解码器的空负载占位缓冲（内容不会被读取） */
    static const SKP_uint8 empty_payload[ 2 ] = { 0, 0 };

    /* ---- 参数检查 ---- */
    if( silk_data == NULL || pcm_out == NULL || pcm_samples == NULL ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }
    *pcm_out     = NULL;
    *pcm_samples = 0;
    if( sample_rate == 0 ) {
        sample_rate = DEFAULT_SAMPLE_RATE;
    }
    /* 解码器 API 采样率的合法区间（见 SKP_Silk_dec_API.c 的范围检查） */
    if( sample_rate < 8000 || sample_rate > MAX_API_FS_KHZ * 1000 ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }

    /* ---- 头部识别：可选 0x02 前缀 + "#!SILK_V3" ---- */
    if( silk_size > 0 && silk_data[ 0 ] == TENCENT_PREFIX ) {
        pos = 1;
    }
    if( silk_size - pos < SILK_HEADER_LEN ||
        memcmp( silk_data + pos, SILK_HEADER, SILK_HEADER_LEN ) != 0 ) {
        return SILK_DLL_ERR_BAD_HEADER;
    }
    pos += SILK_HEADER_LEN;

    /* ---- 创建并初始化解码器（同 Decoder.c） ---- */
    ret = SKP_Silk_SDK_Get_Decoder_Size( &dec_size );
    if( ret != 0 || dec_size <= 0 ) {
        return SILK_DLL_ERR_DECODE;
    }
    dec_state = malloc( (size_t)dec_size );
    frame_buf = (short*)malloc( sizeof( short ) * DEC_FRAME_BUF_SAMPLES );
    if( dec_state == NULL || frame_buf == NULL ) {
        free( dec_state );
        free( frame_buf );
        return SILK_DLL_ERR_ALLOC;
    }
    ret = SKP_Silk_SDK_InitDecoder( dec_state );
    if( ret != 0 ) {
        free( dec_state );
        free( frame_buf );
        return SILK_DLL_ERR_DECODE;
    }

    memset( &dec_ctrl, 0, sizeof( dec_ctrl ) );
    dec_ctrl.API_sampleRate  = sample_rate;
    dec_ctrl.framesPerPacket = 1; /* 首包到达前按 1 帧/包初始化（同 Decoder.c） */

    /* ---- 逐包解码主循环 ---- */
    while( pos + 2 <= silk_size ) {
        /* 读取 int16 小端包长；负值为标准文件的结束标记 */
        short n_bytes = (short)( silk_data[ pos ] | ( silk_data[ pos + 1 ] << 8 ) );
        int   tot_len = 0;
        int   frames  = 0;
        SKP_int16 len;

        pos += 2;
        if( n_bytes < 0 ) {
            break; /* 结束标记 */
        }

        if( n_bytes == 0 ) {
            /* DTX 静默期的 0 长度占位包：按丢包走 PLC 隐藏，生成对应时长输出
             *（对应 Decoder.c 的 lost 分支，网络场景外极少遇到） */
            int i;
            for( i = 0; i < dec_ctrl.framesPerPacket; i++ ) {
                len = 0;
                SKP_Silk_SDK_Decode( dec_state, &dec_ctrl, 1,
                                     empty_payload, 0,
                                     frame_buf + tot_len, &len );
                tot_len += len;
            }
            if( pcm_buf_append( &out, frame_buf, tot_len ) != 0 ) {
                goto alloc_fail;
            }
            continue;
        }

        /* 包长异常（超上限或越过数据尾部）：视为码流损坏，停止解析 */
        if( n_bytes > DEC_MAX_BYTES_PER_FRAME * MAX_INPUT_FRAMES ||
            pos + n_bytes > silk_size ) {
            break;
        }

        /* 解码一个包内的全部 20ms 帧（同 Decoder.c 的 do/while 循环） */
        do {
            len = 0;
            ret = SKP_Silk_SDK_Decode( dec_state, &dec_ctrl, 0,
                                       silk_data + pos, n_bytes,
                                       frame_buf + tot_len, &len );
            if( ret != 0 ) {
                /* 与 Decoder.c 一致：单帧解码错误不中止整体转换 */
            }
            frames++;
            tot_len += len;
            if( frames > MAX_INPUT_FRAMES ) {
                /* 损坏码流防御：帧数超限时丢弃本包已产出数据（同 Decoder.c） */
                tot_len = 0;
                frames  = 0;
            }
        } while( dec_ctrl.moreInternalDecoderFrames );

        pos += n_bytes;
        packets++;

        if( pcm_buf_append( &out, frame_buf, tot_len ) != 0 ) {
            goto alloc_fail;
        }
    }

    free( dec_state );
    free( frame_buf );

    /* 一个有效包都没有解出：输入大概率不是（完整的）SILK v3 码流 */
    if( packets == 0 || out.size == 0 ) {
        free( out.data );
        return SILK_DLL_ERR_DECODE;
    }

    *pcm_out     = out.data;
    *pcm_samples = out.size;
    return SILK_DLL_OK;

alloc_fail:
    free( dec_state );
    free( frame_buf );
    free( out.data );
    return SILK_DLL_ERR_ALLOC;
}

/*======================= 编码 =======================*/

int silk_encode_buffer(
    const short*    pcm,
    int             pcm_samples,
    int             sample_rate,
    int             bit_rate,
    int             tencent,
    unsigned char** silk_out,
    int*            silk_size )
{
    SKP_SILK_SDK_EncControlStruct enc_ctrl;
    SKP_SILK_SDK_EncControlStruct enc_status;
    void*      enc_state = NULL;
    SKP_int32  enc_size  = 0;
    byte_buf_t out       = { NULL, 0, 0 };
    int        frame_samples;
    int        offset;
    int        packets   = 0;
    int        ret;
    unsigned char payload[ ENC_MAX_BYTES_PER_FRAME * MAX_INPUT_FRAMES ];

    /* ---- 参数检查 ---- */
    if( pcm == NULL || silk_out == NULL || silk_size == NULL || pcm_samples <= 0 ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }
    *silk_out  = NULL;
    *silk_size = 0;
    if( sample_rate == 0 ) {
        sample_rate = DEFAULT_SAMPLE_RATE;
    }
    if( bit_rate == 0 ) {
        bit_rate = DEFAULT_BIT_RATE;
    }
    /* SILK 编码器只接受这几档 API 采样率（见 SKP_Silk_enc_API.c） */
    if( sample_rate !=  8000 && sample_rate != 12000 && sample_rate != 16000 &&
        sample_rate != 24000 && sample_rate != 32000 && sample_rate != 44100 &&
        sample_rate != 48000 ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }

    /* 20ms 一帧；不足一帧的输入无法编码 */
    frame_samples = FRAME_LENGTH_MS * sample_rate / 1000;
    if( pcm_samples < frame_samples ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }

    /* ---- 创建并初始化编码器（同 Encoder.c） ---- */
    ret = SKP_Silk_SDK_Get_Encoder_Size( &enc_size );
    if( ret != 0 || enc_size <= 0 ) {
        return SILK_DLL_ERR_ENCODE;
    }
    enc_state = malloc( (size_t)enc_size );
    if( enc_state == NULL ) {
        return SILK_DLL_ERR_ALLOC;
    }
    ret = SKP_Silk_SDK_InitEncoder( enc_state, &enc_status );
    if( ret != 0 ) {
        free( enc_state );
        return SILK_DLL_ERR_ENCODE;
    }

    /* 编码参数与 Encoder.c 的默认值一致：
     * 20ms 包长、复杂度 2（最高）、关闭 FEC/DTX、内部采样率不超过 24k */
    memset( &enc_ctrl, 0, sizeof( enc_ctrl ) );
    enc_ctrl.API_sampleRate        = sample_rate;
    enc_ctrl.maxInternalSampleRate = sample_rate < 24000 ? sample_rate : 24000;
    enc_ctrl.packetSize            = frame_samples;
    enc_ctrl.bitRate               = bit_rate > 0 ? bit_rate : 0;
    enc_ctrl.packetLossPercentage  = 0;
    enc_ctrl.complexity            = 2;
    enc_ctrl.useInBandFEC          = 0;
    enc_ctrl.useDTX                = 0;

    /* ---- 写文件头 ---- */
    if( tencent ) {
        /* QQ/微信兼容：0x02 + "#!SILK_V3"（对应 Encoder.c 的 -tencent 选项） */
        static const unsigned char tencent_header[ 1 + SILK_HEADER_LEN ] =
            { TENCENT_PREFIX, '#', '!', 'S', 'I', 'L', 'K', '_', 'V', '3' };
        if( byte_buf_append( &out, tencent_header, sizeof( tencent_header ) ) != 0 ) {
            goto alloc_fail;
        }
    } else {
        if( byte_buf_append( &out, SILK_HEADER, SILK_HEADER_LEN ) != 0 ) {
            goto alloc_fail;
        }
    }

    /* ---- 逐帧编码主循环：packetSize 为 20ms，每帧编码完即为一个完整包 ---- */
    for( offset = 0; offset + frame_samples <= pcm_samples; offset += frame_samples ) {
        SKP_int16 n_bytes = (SKP_int16)sizeof( payload ); /* 入参 = 缓冲上限 */
        unsigned char len_le[ 2 ];

        ret = SKP_Silk_SDK_Encode( enc_state, &enc_ctrl,
                                   pcm + offset, (SKP_int16)frame_samples,
                                   payload, &n_bytes );
        if( ret != 0 ) {
            free( enc_state );
            free( out.data );
            return SILK_DLL_ERR_ENCODE;
        }

        /* 写 [int16 包长(小端)][负载]（同 Encoder.c） */
        len_le[ 0 ] = (unsigned char)( n_bytes & 0xFF );
        len_le[ 1 ] = (unsigned char)( ( n_bytes >> 8 ) & 0xFF );
        if( byte_buf_append( &out, len_le, 2 ) != 0 ||
            byte_buf_append( &out, payload, n_bytes ) != 0 ) {
            goto alloc_fail;
        }
        packets++;
    }

    /* 标准文件以 int16 -1 结尾；腾讯变体不写结束标记（同 Encoder.c） */
    if( !tencent ) {
        static const unsigned char eos[ 2 ] = { 0xFF, 0xFF };
        if( byte_buf_append( &out, eos, 2 ) != 0 ) {
            goto alloc_fail;
        }
    }

    free( enc_state );

    if( packets == 0 ) {
        /* 理论上不可达（前面已检查 pcm_samples >= frame_samples），防御保留 */
        free( out.data );
        return SILK_DLL_ERR_INVALID_ARG;
    }

    *silk_out  = out.data;
    *silk_size = out.size;
    return SILK_DLL_OK;

alloc_fail:
    free( enc_state );
    free( out.data );
    return SILK_DLL_ERR_ALLOC;
}

/*======================= 文件级 API =======================*/

/* 读取整个文件到内存。返回 0 成功；出参缓冲需调用方 free */
static int read_whole_file( FILE* fp, unsigned char** data_out, int* size_out )
{
    long           file_size;
    unsigned char* data;

    if( fseek( fp, 0, SEEK_END ) != 0 ) {
        return SILK_DLL_ERR_IO;
    }
    file_size = ftell( fp );
    /* 语音文件通常仅几十 KB；拒绝空文件与超过 INT_MAX 的输入 */
    if( file_size <= 0 || file_size > 0x7FFFFFF0L ) {
        return SILK_DLL_ERR_IO;
    }
    if( fseek( fp, 0, SEEK_SET ) != 0 ) {
        return SILK_DLL_ERR_IO;
    }
    data = (unsigned char*)malloc( (size_t)file_size );
    if( data == NULL ) {
        return SILK_DLL_ERR_ALLOC;
    }
    if( fread( data, 1, (size_t)file_size, fp ) != (size_t)file_size ) {
        free( data );
        return SILK_DLL_ERR_IO;
    }
    *data_out = data;
    *size_out = (int)file_size;
    return SILK_DLL_OK;
}

/* 解码文件公共实现：输入/输出文件句柄已打开 */
static int decode_file_impl( FILE* in_fp, FILE* out_fp, int sample_rate )
{
    unsigned char* silk_data = NULL;
    int            silk_size = 0;
    short*         pcm       = NULL;
    int            samples   = 0;
    int            ret;

    ret = read_whole_file( in_fp, &silk_data, &silk_size );
    if( ret != SILK_DLL_OK ) {
        return ret;
    }
    ret = silk_decode_buffer( silk_data, silk_size, sample_rate, &pcm, &samples );
    free( silk_data );
    if( ret != SILK_DLL_OK ) {
        return ret;
    }
    if( fwrite( pcm, sizeof( short ), (size_t)samples, out_fp ) != (size_t)samples ) {
        ret = SILK_DLL_ERR_IO;
    }
    silk_free( pcm );
    return ret;
}

/* 编码文件公共实现：输入/输出文件句柄已打开 */
static int encode_file_impl( FILE* in_fp, FILE* out_fp,
                             int sample_rate, int bit_rate, int tencent )
{
    unsigned char* pcm_raw   = NULL;
    int            pcm_bytes = 0;
    unsigned char* silk_data = NULL;
    int            silk_size = 0;
    int            ret;

    ret = read_whole_file( in_fp, &pcm_raw, &pcm_bytes );
    if( ret != SILK_DLL_OK ) {
        return ret;
    }
    ret = silk_encode_buffer( (const short*)pcm_raw, pcm_bytes / 2,
                              sample_rate, bit_rate, tencent,
                              &silk_data, &silk_size );
    free( pcm_raw );
    if( ret != SILK_DLL_OK ) {
        return ret;
    }
    if( fwrite( silk_data, 1, (size_t)silk_size, out_fp ) != (size_t)silk_size ) {
        ret = SILK_DLL_ERR_IO;
    }
    silk_free( silk_data );
    return ret;
}

int silk_decode_file( const char* silk_path, const char* pcm_path, int sample_rate )
{
    FILE* in_fp;
    FILE* out_fp;
    int   ret;

    if( silk_path == NULL || pcm_path == NULL ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }
    in_fp = fopen( silk_path, "rb" );
    if( in_fp == NULL ) {
        return SILK_DLL_ERR_IO;
    }
    out_fp = fopen( pcm_path, "wb" );
    if( out_fp == NULL ) {
        fclose( in_fp );
        return SILK_DLL_ERR_IO;
    }
    ret = decode_file_impl( in_fp, out_fp, sample_rate );
    fclose( in_fp );
    fclose( out_fp );
    return ret;
}

int silk_encode_file( const char* pcm_path, const char* silk_path,
                      int sample_rate, int bit_rate, int tencent )
{
    FILE* in_fp;
    FILE* out_fp;
    int   ret;

    if( pcm_path == NULL || silk_path == NULL ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }
    in_fp = fopen( pcm_path, "rb" );
    if( in_fp == NULL ) {
        return SILK_DLL_ERR_IO;
    }
    out_fp = fopen( silk_path, "wb" );
    if( out_fp == NULL ) {
        fclose( in_fp );
        return SILK_DLL_ERR_IO;
    }
    ret = encode_file_impl( in_fp, out_fp, sample_rate, bit_rate, tencent );
    fclose( in_fp );
    fclose( out_fp );
    return ret;
}

#ifdef _WIN32

int silk_decode_file_w( const wchar_t* silk_path, const wchar_t* pcm_path, int sample_rate )
{
    FILE* in_fp;
    FILE* out_fp;
    int   ret;

    if( silk_path == NULL || pcm_path == NULL ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }
    in_fp = _wfopen( silk_path, L"rb" );
    if( in_fp == NULL ) {
        return SILK_DLL_ERR_IO;
    }
    out_fp = _wfopen( pcm_path, L"wb" );
    if( out_fp == NULL ) {
        fclose( in_fp );
        return SILK_DLL_ERR_IO;
    }
    ret = decode_file_impl( in_fp, out_fp, sample_rate );
    fclose( in_fp );
    fclose( out_fp );
    return ret;
}

int silk_encode_file_w( const wchar_t* pcm_path, const wchar_t* silk_path,
                        int sample_rate, int bit_rate, int tencent )
{
    FILE* in_fp;
    FILE* out_fp;
    int   ret;

    if( pcm_path == NULL || silk_path == NULL ) {
        return SILK_DLL_ERR_INVALID_ARG;
    }
    in_fp = _wfopen( pcm_path, L"rb" );
    if( in_fp == NULL ) {
        return SILK_DLL_ERR_IO;
    }
    out_fp = _wfopen( silk_path, L"wb" );
    if( out_fp == NULL ) {
        fclose( in_fp );
        return SILK_DLL_ERR_IO;
    }
    ret = encode_file_impl( in_fp, out_fp, sample_rate, bit_rate, tencent );
    fclose( in_fp );
    fclose( out_fp );
    return ret;
}

#endif /* _WIN32 */
