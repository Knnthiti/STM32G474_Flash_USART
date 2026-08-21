@echo off
setlocal

set "SRC=%~dp0Flash_program_C.c"
set "OUT=%~dp0Flash_program_C.exe"
set "TMP=%~dp0"
set "TEMP=%~dp0"
set "TMPDIR=%~dp0"
set "BUILD_LOG=%~dp0build_mingw.log"

where gcc >nul 2>nul
if errorlevel 1 (
    echo gcc.exe was not found. Install MinGW-w64 or add gcc to PATH.
    exit /b 1
)

echo Building Flash_program_C.exe...
gcc -std=c11 -Wall -Wextra -D_WIN32_WINNT=0x0601 -o "%OUT%" "%SRC%" -mwindows -lcomctl32 -lcomdlg32 -lgdi32 -luser32 > "%BUILD_LOG%" 2>&1
set "BUILD_STATUS=%errorlevel%"

if not "%BUILD_STATUS%"=="0" (
    echo Build failed. Compiler output:
    type "%BUILD_LOG%"
    exit /b %BUILD_STATUS%
)

echo Build complete: "%OUT%"
exit /b 0
