/***********************************************************************
 * test_dll.c —— silkcodec.dll 端到端自测程序
 *
 * 通过 LoadLibrary/GetProcAddress 动态加载 DLL（同时验证导出名是否
 * 正确），执行以下检查：
 *
 *   1. 全部高层 API 与代表性的底层 SDK API 均可解析；
 *   2. 合成 1 秒 440Hz 正弦 PCM（24kHz），编码为腾讯格式（0x02 前缀），
 *      校验头部字节；再解码回 PCM，校验样本数、能量与过零率（频率）；
 *   3. 标准格式（无前缀、-1 结尾）编码与解码往返；
 *   4. 文件级 API 往返，且输出与内存级 API 逐字节一致；
 *   5. 宽字符 API（含中文路径）；
 *   6. 错误路径：非法头部、非法采样率应返回对应错误码。
 *
 * 控制台输出使用英文（避免非 UTF-8 代码页下乱码），全部通过时
 * 退出码为 0，任何一步失败退出码为 1。
 ***********************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 与 silk_dll.h 一致的错误码（独立定义，验证 ABI 而非共享头文件） */
#define SILK_DLL_OK               0
#define SILK_DLL_ERR_INVALID_ARG -1
#define SILK_DLL_ERR_BAD_HEADER  -2

/* 高层 API 的函数指针类型（cdecl） */
typedef const char* (*fn_version_t)( void );
typedef int  (*fn_decode_buffer_t)( const unsigned char*, int, int, short**, int* );
typedef int  (*fn_encode_buffer_t)( const short*, int, int, int, int, unsigned char**, int* );
typedef void (*fn_free_t)( void* );
typedef int  (*fn_decode_file_t)( const char*, const char*, int );
typedef int  (*fn_encode_file_t)( const char*, const char*, int, int, int );
typedef int  (*fn_decode_file_w_t)( const wchar_t*, const wchar_t*, int );
typedef int  (*fn_encode_file_w_t)( const wchar_t*, const wchar_t*, int, int, int );
typedef const char* (*fn_sdk_version_t)( void );

static int g_failures = 0;

/* 检查辅助宏：条件不成立时打印并累计失败 */
#define CHECK( cond, msg )                                             \
    do {                                                               \
        if( cond ) {                                                   \
            printf( "  [PASS] %s\n", msg );                            \
        } else {                                                       \
            printf( "  [FAIL] %s\n", msg );                            \
            g_failures++;                                              \
        }                                                              \
    } while( 0 )

/* 读取整个文件（测试用），失败返回 NULL */
static unsigned char* read_file( const char* path, long* size_out )
{
    FILE* fp = fopen( path, "rb" );
    unsigned char* data;
    long size;
    if( fp == NULL ) return NULL;
    fseek( fp, 0, SEEK_END );
    size = ftell( fp );
    fseek( fp, 0, SEEK_SET );
    data = (unsigned char*)malloc( (size_t)size );
    if( data != NULL && fread( data, 1, (size_t)size, fp ) != (size_t)size ) {
        free( data );
        data = NULL;
    }
    fclose( fp );
    if( data != NULL ) *size_out = size;
    return data;
}

int main( int argc, char* argv[] )
{
    const char* dll_path = ( argc > 1 ) ? argv[ 1 ] : "silkcodec.dll";
    HMODULE h;

    fn_version_t       p_version;
    fn_decode_buffer_t p_decode_buffer;
    fn_encode_buffer_t p_encode_buffer;
    fn_free_t          p_free;
    fn_decode_file_t   p_decode_file;
    fn_encode_file_t   p_encode_file;
    fn_decode_file_w_t p_decode_file_w;
    fn_encode_file_w_t p_encode_file_w;
    fn_sdk_version_t   p_sdk_version;

    enum { SAMPLE_RATE = 24000, DURATION_S = 1 };
    const int num_samples = SAMPLE_RATE * DURATION_S;
    short* pcm_in;
    unsigned char* silk_tc  = NULL; int silk_tc_size  = 0;
    unsigned char* silk_std = NULL; int silk_std_size = 0;
    short* pcm_dec = NULL; int dec_samples = 0;
    int i, ret;

    printf( "=== silkcodec.dll self test ===\n" );
    printf( "DLL: %s\n", dll_path );

    /* ---- 1. 动态加载与导出解析 ---- */
    h = LoadLibraryA( dll_path );
    if( h == NULL ) {
        printf( "[FAIL] LoadLibrary failed, GetLastError=%lu\n", GetLastError() );
        return 1;
    }
    printf( "[1] Resolving exports...\n" );
    p_version       = (fn_version_t)      GetProcAddress( h, "silk_dll_version" );
    p_decode_buffer = (fn_decode_buffer_t)GetProcAddress( h, "silk_decode_buffer" );
    p_encode_buffer = (fn_encode_buffer_t)GetProcAddress( h, "silk_encode_buffer" );
    p_free          = (fn_free_t)         GetProcAddress( h, "silk_free" );
    p_decode_file   = (fn_decode_file_t)  GetProcAddress( h, "silk_decode_file" );
    p_encode_file   = (fn_encode_file_t)  GetProcAddress( h, "silk_encode_file" );
    p_decode_file_w = (fn_decode_file_w_t)GetProcAddress( h, "silk_decode_file_w" );
    p_encode_file_w = (fn_encode_file_w_t)GetProcAddress( h, "silk_encode_file_w" );
    p_sdk_version   = (fn_sdk_version_t)  GetProcAddress( h, "SKP_Silk_SDK_get_version" );

    CHECK( p_version && p_decode_buffer && p_encode_buffer && p_free &&
           p_decode_file && p_encode_file && p_decode_file_w && p_encode_file_w,
           "all high-level exports resolved" );
    CHECK( p_sdk_version != NULL, "low-level SDK export resolved (SKP_Silk_SDK_get_version)" );
    if( g_failures ) return 1;

    printf( "  DLL version: %s, SILK SDK version: %s\n", p_version(), p_sdk_version() );

    /* ---- 2. 合成测试信号：1 秒 440Hz 正弦，幅度 8000 ---- */
    pcm_in = (short*)malloc( sizeof( short ) * num_samples );
    if( pcm_in == NULL ) return 1;
    for( i = 0; i < num_samples; i++ ) {
        pcm_in[ i ] = (short)( 8000.0 * sin( 2.0 * 3.14159265358979 * 440.0 * i / SAMPLE_RATE ) );
    }

    /* ---- 3. 腾讯格式（QQ/微信）编码与头部校验 ---- */
    printf( "[2] Encode (tencent format)...\n" );
    ret = p_encode_buffer( pcm_in, num_samples, SAMPLE_RATE, 25000, 1, &silk_tc, &silk_tc_size );
    CHECK( ret == SILK_DLL_OK, "silk_encode_buffer(tencent=1) returns OK" );
    CHECK( silk_tc_size > 10, "encoded size is positive" );
    CHECK( silk_tc != NULL && silk_tc[ 0 ] == 0x02 &&
           memcmp( silk_tc + 1, "#!SILK_V3", 9 ) == 0,
           "tencent header is 0x02 + #!SILK_V3" );
    printf( "  encoded: %d samples -> %d bytes\n", num_samples, silk_tc_size );

    /* ---- 4. 解码往返与信号质量校验 ---- */
    printf( "[3] Decode (tencent format) and verify signal...\n" );
    ret = p_decode_buffer( silk_tc, silk_tc_size, SAMPLE_RATE, &pcm_dec, &dec_samples );
    CHECK( ret == SILK_DLL_OK, "silk_decode_buffer returns OK" );
    CHECK( dec_samples == num_samples, "decoded sample count == encoded sample count" );
    if( ret == SILK_DLL_OK && dec_samples > 0 ) {
        /* 能量与过零率检查：跳过前 1/4（编解码器启动瞬态） */
        double in_rms = 0.0, out_rms = 0.0;
        int zc = 0, start = dec_samples / 4;
        double zc_per_sec, expect_zc = 2.0 * 440.0;
        for( i = start; i < dec_samples; i++ ) {
            in_rms  += (double)pcm_in[ i ]  * pcm_in[ i ];
            out_rms += (double)pcm_dec[ i ] * pcm_dec[ i ];
            if( i > start && ( ( pcm_dec[ i - 1 ] < 0 ) != ( pcm_dec[ i ] < 0 ) ) ) {
                zc++;
            }
        }
        in_rms  = sqrt( in_rms  / ( dec_samples - start ) );
        out_rms = sqrt( out_rms / ( dec_samples - start ) );
        zc_per_sec = (double)zc * SAMPLE_RATE / ( dec_samples - start );
        printf( "  input RMS=%.0f, decoded RMS=%.0f, zero-crossings=%.0f/s (expect ~%.0f)\n",
                in_rms, out_rms, zc_per_sec, expect_zc );
        CHECK( out_rms > in_rms * 0.2 && out_rms < in_rms * 2.0,
               "decoded energy within reasonable range of input" );
        CHECK( zc_per_sec > expect_zc * 0.8 && zc_per_sec < expect_zc * 1.2,
               "decoded frequency (zero-crossing rate) matches 440Hz" );
    }

    /* ---- 5. 标准格式编码/解码往返 ---- */
    printf( "[4] Encode/decode (standard format)...\n" );
    ret = p_encode_buffer( pcm_in, num_samples, SAMPLE_RATE, 25000, 0, &silk_std, &silk_std_size );
    CHECK( ret == SILK_DLL_OK, "silk_encode_buffer(tencent=0) returns OK" );
    CHECK( silk_std != NULL && memcmp( silk_std, "#!SILK_V3", 9 ) == 0,
           "standard header has no 0x02 prefix" );
    CHECK( silk_std_size > 2 &&
           silk_std[ silk_std_size - 2 ] == 0xFF && silk_std[ silk_std_size - 1 ] == 0xFF,
           "standard stream ends with int16 -1 marker" );
    if( ret == SILK_DLL_OK ) {
        short* pcm2 = NULL; int n2 = 0;
        ret = p_decode_buffer( silk_std, silk_std_size, SAMPLE_RATE, &pcm2, &n2 );
        CHECK( ret == SILK_DLL_OK && n2 == num_samples, "standard stream decodes to same length" );
        if( ret == SILK_DLL_OK ) p_free( pcm2 );
    }

    /* ---- 6. 文件级 API 往返，与内存级结果逐字节比对 ---- */
    printf( "[5] File API round trip...\n" );
    {
        const char* silk_file = "test_out.silk";
        const char* pcm_file  = "test_out.pcm";
        FILE* fp = fopen( silk_file, "wb" );
        long fsize = 0;
        unsigned char* fdata;
        if( fp != NULL ) {
            fwrite( silk_tc, 1, (size_t)silk_tc_size, fp );
            fclose( fp );
        }
        ret = p_decode_file( silk_file, pcm_file, SAMPLE_RATE );
        CHECK( ret == SILK_DLL_OK, "silk_decode_file returns OK" );
        fdata = read_file( pcm_file, &fsize );
        CHECK( fdata != NULL && fsize == (long)dec_samples * 2 &&
               memcmp( fdata, pcm_dec, (size_t)fsize ) == 0,
               "file API output identical to buffer API output" );
        free( fdata );
        remove( silk_file );
        remove( pcm_file );
    }

    /* ---- 7. 宽字符 API（中文路径） ---- */
    printf( "[6] Wide-char file API (Chinese path)...\n" );
    {
        /* 源文件为 UTF-8 且以 /utf-8 编译，L"..." 字面量可正确携带中文 */
        const wchar_t* pcm_w  = L"测试输入_中文.pcm";  /* 测试输入_中文.pcm */
        const wchar_t* silk_w = L"测试输出_中文.silk"; /* 测试输出_中文.silk */
        FILE* fp = _wfopen( pcm_w, L"wb" );
        if( fp != NULL ) {
            fwrite( pcm_in, sizeof( short ), num_samples, fp );
            fclose( fp );
        }
        ret = p_encode_file_w( pcm_w, silk_w, SAMPLE_RATE, 25000, 1 );
        CHECK( ret == SILK_DLL_OK, "silk_encode_file_w (Chinese path) returns OK" );
        {
            const wchar_t* pcm2_w = L"测试回转_中文.pcm"; /* 测试回转_中文.pcm */
            ret = p_decode_file_w( silk_w, pcm2_w, SAMPLE_RATE );
            CHECK( ret == SILK_DLL_OK, "silk_decode_file_w (Chinese path) returns OK" );
            _wremove( pcm2_w );
        }
        _wremove( pcm_w );
        _wremove( silk_w );
    }

    /* ---- 8. 错误路径 ---- */
    printf( "[7] Error paths...\n" );
    {
        short* dummy_pcm = NULL; int dummy_n = 0;
        unsigned char bad[ 16 ] = "RIFFxxxxWAVEfmt";
        unsigned char* dummy_out = NULL; int dummy_size = 0;
        ret = p_decode_buffer( bad, sizeof( bad ), SAMPLE_RATE, &dummy_pcm, &dummy_n );
        CHECK( ret == SILK_DLL_ERR_BAD_HEADER, "non-silk input rejected with ERR_BAD_HEADER" );
        ret = p_encode_buffer( pcm_in, num_samples, 11025, 0, 0, &dummy_out, &dummy_size );
        CHECK( ret == SILK_DLL_ERR_INVALID_ARG, "unsupported encode sample rate rejected" );
        ret = p_decode_buffer( NULL, 0, SAMPLE_RATE, &dummy_pcm, &dummy_n );
        CHECK( ret == SILK_DLL_ERR_INVALID_ARG, "NULL input rejected" );
    }

    p_free( pcm_dec );
    p_free( silk_tc );
    p_free( silk_std );
    free( pcm_in );
    FreeLibrary( h );

    printf( "=== %s (%d failure(s)) ===\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures );
    return g_failures == 0 ? 0 : 1;
}
