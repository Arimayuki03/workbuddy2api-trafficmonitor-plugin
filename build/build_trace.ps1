# build_trace.ps1 — 带调用追踪的诊断版编译（WB2API_TRACE_BUILD + trace.cpp）。排查用。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$out = Join-Path $root "build\out\Release"
$mods = @('common','logger','settings','http','procctl','autostart','worker','item','plugin','dialogs','trace')
$srcList = ($mods | ForEach-Object { Join-Path $root "src\$_.cpp" }) -join ' '
$objs = ($mods | ForEach-Object { "$_.obj" }) -join ' '
$cppFlags = "/nologo /std:c++17 /EHsc /W3 /utf-8 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /O2 /MT /DWB2API_TRACE_BUILD /DWB2API_TRACE_AB_EMPTY /I`"$root\include`" /I`"$root\third_party`" /I`"$root\res`""
$lines = @(
    '@echo off',
    "call `"$vcvars`" || exit /b 1",
    "cd /d `"$out`" || exit /b 1",
    "cl.exe $cppFlags /c $srcList || exit /b 3",
    "link.exe /nologo /DLL /MACHINE:X64 /SUBSYSTEM:WINDOWS /OUT:WorkBuddy2ApiPlugin.dll $objs plugin.res user32.lib gdi32.lib shell32.lib ole32.lib shlwapi.lib iphlpapi.lib winhttp.lib comctl32.lib uxtheme.lib ws2_32.lib advapi32.lib || exit /b 4",
    'echo TRACE-BUILD-OK'
)
$batFile = Join-Path $env:TEMP "wb2api_trace_build.bat"
[System.IO.File]::WriteAllText($batFile, ($lines -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)
& cmd.exe /c "`"$batFile`""
if ($LASTEXITCODE -ne 0) { throw "trace build failed" }
