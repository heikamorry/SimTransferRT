@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
python tools\run_diagnostics.py %*
exit /b %ERRORLEVEL%
