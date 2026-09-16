@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "D:\Code\workbuddy2api-trafficmonitor-plugin\build\out\Release"
cl.exe /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"..\..\..\include" /I"..\..\..\src" ..\..\test_harness.cpp /Fe:test_harness.exe /link user32.lib gdi32.lib || exit /b 2
test_harness.exe WorkBuddy2ApiPlugin.dll
