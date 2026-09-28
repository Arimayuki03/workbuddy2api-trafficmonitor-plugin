@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0out\Release"
rem Run build\build.ps1 first to produce the objs below. worker.obj excluded: tip_unit.cpp includes worker.cpp itself.
cl.exe /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"..\..\..\include" /I"..\..\..\third_party" /c ..\..\tip_unit.cpp /Fo:tip_unit.obj || exit /b 2
link.exe /nologo /SUBSYSTEM:CONSOLE /OUT:tip_unit.exe tip_unit.obj common.obj logger.obj settings.obj http.obj procctl.obj autostart.obj item.obj trace.obj plugin.obj dialogs.obj user32.lib gdi32.lib winhttp.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib iphlpapi.lib shlwapi.lib comctl32.lib uxtheme.lib || exit /b 3
tip_unit.exe
