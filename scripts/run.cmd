@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
set "BUILD=windows-debug"
if /I "%~1"=="Release" (
    set "BUILD=windows-v2-release"
    if not exist "build\windows-v2-release\SimTransfer.exe" (
        if exist "build\windows-release\SimTransfer.exe" set "BUILD=windows-release"
    )
)
if not exist "build\%BUILD%\SimTransfer.exe" (
    echo ERROR: "build\%BUILD%\SimTransfer.exe" was not found.
    if /I "%~1"=="Release" (
        echo Run scripts\build-v2.cmd Release first.
    ) else (
        echo Run scripts\build.cmd first.
    )
    exit /b 1
)
"build\%BUILD%\SimTransfer.exe"
exit /b %ERRORLEVEL%
