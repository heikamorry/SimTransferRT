@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
if not exist "build\windows-debug\SimTransferCli.exe" (
    echo Run scripts\build.cmd first.
    exit /b 1
)
python tools\test_protocol.py
if errorlevel 1 exit /b 1
python tools\run_e2e.py --sender build\windows-debug\SimTransferCli.exe --out build\network-tests
exit /b %ERRORLEVEL%
