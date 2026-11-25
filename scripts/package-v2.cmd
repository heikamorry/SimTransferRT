@echo off
setlocal
cd /d "%~dp0.."
call scripts\build-v2.cmd
if errorlevel 1 exit /b 1
call scripts\env.cmd
if errorlevel 1 exit /b 1
cmake --build build\windows-v2-release --target SimSetupActions
if errorlevel 1 exit /b 1
python installer\package.py %*
exit /b %errorlevel%
