# CMakeLists.txt —— silkcodec 动态库（SILK v3 编解码器封装）
#
# 用法（Windows / MSVC 示例）：
#   cmake -S windows-dll -B windows-dll/build-cmake -A x64
#   cmake --build windows-dll/build-cmake --config Release
#
# 非 Windows 平台也可构建为共享库（.so/.dylib），
# 但 silk_dll.def 与版本资源仅在 Windows/MSVC 下参与编译。

cmake_minimum_required(VERSION 3.15)
project(silkcodec C)

set(SILK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../silk)

# SILK 定点核心全部 .c（ARM 汇编 .S 仅用于交叉编译，此处不参与）
file(GLOB SILK_SOURCES ${SILK_ROOT}/src/*.c)

set(SILKCODEC_SOURCES silk_dll.c ${SILK_SOURCES})
if(WIN32)
    # 通过 .def 追加导出底层 SKP_Silk_SDK_* 原生函数
    list(APPEND SILKCODEC_SOURCES silk_dll.def)
endif()
if(MSVC)
    list(APPEND SILKCODEC_SOURCES silk_dll.rc)
endif()

add_library(silkcodec SHARED ${SILKCODEC_SOURCES})
target_include_directories(silkcodec PRIVATE ${SILK_ROOT}/interface ${SILK_ROOT}/src)
target_compile_definitions(silkcodec PRIVATE SILK_DLL_EXPORTS _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    # /utf-8：源码与执行字符集均为 UTF-8（本仓库源文件为 UTF-8 无 BOM）
    target_compile_options(silkcodec PRIVATE /utf-8 /W3)
    # 静态 CRT，避免目标机器缺 VC 运行库
    set_property(TARGET silkcodec PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 端到端自测程序（依赖 LoadLibrary，仅 Windows）
if(WIN32)
    add_executable(test_dll test/test_dll.c)
    if(MSVC)
        target_compile_options(test_dll PRIVATE /utf-8 /W3)
        set_property(TARGET test_dll PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()
