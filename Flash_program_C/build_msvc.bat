@echo off
setlocal

set "SRC=%~dp0Flash_program_C.c"
set "OUT=%~dp0Flash_program_C.exe"

where cl >nul 2>nul
if errorlevel 1 (
    echo cl.exe was not found. Open "Developer Command Prompt for VS" and run this file again.
    exit /b 1
)

cl /nologo /W4 /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 /Fe"%OUT%" "%SRC%" user32.lib gdi32.lib comdlg32.lib comctl32.lib
exit /b %errorlevel%
