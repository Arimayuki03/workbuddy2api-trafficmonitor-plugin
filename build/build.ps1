# build.ps1 — 用 VS2022 BuildTools 的 cl/rc/link 直接编译 x64 DLL。
# 产物：build\out\Release\WorkBuddy2ApiPlugin.dll。用法：powershell -File build\build.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - install VS2022 C++ BuildTools" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "no VS instance with VC tools" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "missing $vcvars" }

$cfg = if ($env:CFG) { $env:CFG } else { 'Release' }
$out = Join-Path $root "build\out\$cfg"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# 模块表只在这里维护一份：$srcList 与 $objs 都从 $mods 派生（此前两处各硬编码一份，
# 新增/改名模块时改一漏一就会编译或链接失败），参照 build_trace.ps1 的单份范式。
$mods = @('common','logger','settings','http','procctl','autostart','worker','item','plugin','dialogs','trace')
$srcList = ($mods | ForEach-Object { Join-Path $root "src\$_.cpp" }) -join ' '
$objs = ($mods | ForEach-Object { "$_.obj" }) -join ' '

$cppFlags = "/nologo /std:c++17 /EHsc /W3 /utf-8 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /O2 /MT /I`"$root\include`" /I`"$root\third_party`" /I`"$root\res`""
$rcFlags  = "/nologo /DUNICODE /D_UNICODE /fo plugin.res /I`"$root\res`" /I`"$root\include`""

$lines = @(
    '@echo off',
    "call `"$vcvars`" || exit /b 1",
    "cd /d `"$out`" || exit /b 1",
    "rc.exe $rcFlags `"$root\res\plugin.rc`" || exit /b 2",
    "cl.exe $cppFlags /c $srcList || exit /b 3",
    "link.exe /nologo /DLL /MACHINE:X64 /SUBSYSTEM:WINDOWS /OUT:WorkBuddy2ApiPlugin.dll $objs plugin.res user32.lib gdi32.lib shell32.lib ole32.lib shlwapi.lib iphlpapi.lib winhttp.lib comctl32.lib uxtheme.lib ws2_32.lib advapi32.lib || exit /b 4",
    'echo BUILD-OK'
)
$bat = ($lines -join "`r`n") + "`r`n"
$batFile = Join-Path $env:TEMP "wb2api_tm_build.bat"
[System.IO.File]::WriteAllText($batFile, $bat, [System.Text.Encoding]::ASCII)
& cmd.exe /c "`"$batFile`""
$code = $LASTEXITCODE
if ($code -ne 0) { throw "build failed (exit $code)" }
Write-Host "OK: $out\WorkBuddy2ApiPlugin.dll"
