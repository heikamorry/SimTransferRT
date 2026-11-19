@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
set "BUILD=windows-debug"
if /I "%~1"=="Release" set "BUILD=windows-release"
if not exist "build\%BUILD%\SimTransferCli.exe" (
    echo Run scripts\build.cmd %~1 first.
    exit /b 1
)
echo Network test build: %BUILD%
python -m unittest discover -s tools -p "test_*.py"
if errorlevel 1 exit /b 1
python tools\run_e2e.py --sender build\%BUILD%\SimTransferCli.exe --out artifacts\udp-diagnostics\network-%BUILD%
exit /b %ERRORLEVEL%
