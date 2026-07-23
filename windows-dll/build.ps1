<#
.SYNOPSIS
    一键构建 silkcodec.dll（SILK v3 编解码动态库）及其自测程序。

.DESCRIPTION
    使用本机最新版 Visual Studio 的 MSVC 工具链（通过 vswhere 自动定位，
    也可用 -VcVarsAll 手动指定 vcvarsall.bat 路径）编译：
      - build\<arch>\silkcodec.dll   动态库（静态 CRT /MT，无 VC 运行库依赖）
      - build\<arch>\silkcodec.lib   导入库
      - build\<arch>\test_dll.exe    端到端自测程序
    构建完成后默认自动运行自测（可用 -SkipTest 跳过）。

.PARAMETER Arch
    目标架构：x64 / x86 / both（默认 both）。

.PARAMETER VcVarsAll
    可选，手动指定 vcvarsall.bat 完整路径；不指定时用 vswhere 探测最新 VS。

.PARAMETER SkipTest
    只构建，不运行自测程序。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Arch x64 -SkipTest
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'both')]
    [string]$Arch = 'both',

    [string]$VcVarsAll = '',

    [switch]$SkipTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---- 路径准备 ----
# 注意：Split-Path 的 -LiteralPath 与 -Parent 在 PowerShell 7 中属于不同参数集，
# 不能同时使用，这里用内置的 $PSScriptRoot 取脚本所在目录
$scriptDir = $PSScriptRoot
$silkDir   = Join-Path (Split-Path -Parent $scriptDir) 'silk'
$buildRoot = Join-Path $scriptDir 'build'

if (-not (Test-Path -LiteralPath (Join-Path $silkDir 'interface\SKP_Silk_SDK_API.h'))) {
    throw "silk source not found: $silkDir (expected repo layout <repo>\silk + <repo>\windows-dll)"
}

# ---- 定位 vcvarsall.bat：优先手动指定，其次 vswhere 探测最新 VS ----
if ([string]::IsNullOrEmpty($VcVarsAll)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        # -products * 兼容 BuildTools；要求带 C++ 工具集组件
        $vsArgs = @(
            '-latest', '-products', '*',
            '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-property', 'installationPath'
        )
        # 先让 vswhere 完整执行完再取首行：管道中途 Select-Object -First 会提前
        # 终止 vswhere，导致严格模式下 $LASTEXITCODE 未设置而报错
        $vsOutput = @(& $vswhere @vsArgs)
        $installPath = $vsOutput | Select-Object -First 1
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrEmpty($installPath)) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvarsall.bat'
            if (Test-Path -LiteralPath $candidate) {
                $VcVarsAll = $candidate
            }
        }
    }
}
if ([string]::IsNullOrEmpty($VcVarsAll) -or -not (Test-Path -LiteralPath $VcVarsAll)) {
    throw 'vcvarsall.bat not found. Install Visual Studio with C++ workload, or pass -VcVarsAll <path>.'
}
Write-Host "Using MSVC environment: $VcVarsAll"

# ---- 单架构构建函数 ----
function Build-OneArch {
    param([string]$TargetArch)

    $outDir = Join-Path $buildRoot $TargetArch
    $objDir = Join-Path $buildRoot ("obj\" + $TargetArch)
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    # 生成临时批处理：先 call vcvarsall 配好环境，再依次 rc / cl。
    # 内容全为 ASCII 路径命令，写为 ASCII 编码避免 OEM 代码页问题。
    $cmdFile = Join-Path $buildRoot ("_build_" + $TargetArch + ".cmd")
    $lines = @(
        '@echo off',
        ('call "{0}" {1}' -f $VcVarsAll, $TargetArch),
        'if errorlevel 1 exit /b 1',
        ('rc /nologo /fo "{0}\silk_dll.res" "{1}\silk_dll.rc"' -f $objDir, $scriptDir),
        'if errorlevel 1 exit /b 1',
        # /MT 静态 CRT；/utf-8 显式源码与执行字符集；/LD 生成 DLL；
        # .def 追加导出底层 SKP_Silk_SDK_* 函数
        (('cl /nologo /O2 /MT /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /DSILK_DLL_EXPORTS ' +
          '/I"{0}\interface" /I"{0}\src" ' +
          '/LD /Fe:"{1}\silkcodec.dll" /Fo"{2}\\" ' +
          '"{3}\silk_dll.c" "{0}\src\*.c" "{2}\silk_dll.res" ' +
          '/link /DEF:"{3}\silk_dll.def"') -f $silkDir, $outDir, $objDir, $scriptDir),
        'if errorlevel 1 exit /b 1',
        (('cl /nologo /O2 /MT /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS ' +
          '/Fe:"{0}\test_dll.exe" /Fo"{1}\\" "{2}\test\test_dll.c"') -f $outDir, $objDir, $scriptDir),
        'if errorlevel 1 exit /b 1'
    )
    [System.IO.File]::WriteAllLines($cmdFile, $lines, [System.Text.Encoding]::ASCII)

    Write-Host ""
    Write-Host "===== Building $TargetArch =====" -ForegroundColor Cyan
    & cmd.exe @('/d', '/c', $cmdFile)
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $TargetArch (exit code $LASTEXITCODE)"
    }
    Remove-Item -LiteralPath $cmdFile -Force

    Write-Host "Output: $outDir\silkcodec.dll" -ForegroundColor Green

    if (-not $SkipTest) {
        Write-Host "----- Running self test ($TargetArch) -----"
        Push-Location -LiteralPath $outDir
        try {
            & (Join-Path $outDir 'test_dll.exe') (Join-Path $outDir 'silkcodec.dll')
            if ($LASTEXITCODE -ne 0) {
                throw "Self test failed for $TargetArch (exit code $LASTEXITCODE)"
            }
        }
        finally {
            Pop-Location
        }
    }
}

# ---- 执行 ----
$targets = if ($Arch -eq 'both') { @('x64', 'x86') } else { @($Arch) }
foreach ($t in $targets) {
    Build-OneArch -TargetArch $t
}

Write-Host ""
Write-Host "All done." -ForegroundColor Green
