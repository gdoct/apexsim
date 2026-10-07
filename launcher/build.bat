@echo off
rem Builds launcher.exe with MSVC. Run from anywhere; output goes to launcher\out.
rem Uses the first Visual Studio 2022 edition found unless cl.exe is already on PATH.
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    for %%E in (Enterprise Professional Community BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            call "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" >nul
            goto :build
        )
    )
    echo Could not find MSVC. Run this from a "x64 Native Tools" prompt.
    exit /b 1
)
:build
if not exist out mkdir out
rc /nologo /fo out\launcher.res launcher.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /GL /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE ^
   /Fo:out\ /Fe:out\launcher.exe main.cpp ^
   /link /SUBSYSTEM:WINDOWS /LTCG /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
   out\launcher.res user32.lib gdi32.lib gdiplus.lib comctl32.lib shell32.lib ole32.lib ws2_32.lib uuid.lib
exit /b %errorlevel%
