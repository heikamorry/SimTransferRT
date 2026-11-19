@echo off
setlocal
cd /d "%~dp0.."
call scripts\build-v2.cmd
if errorlevel 1 exit /b 1
call scripts\env.cmd
if errorlevel 1 exit /b 1
cmake --install build\windows-v2-release --prefix "%CD%\dist\v2-release"
if errorlevel 1 exit /b 1
copy /Y build\windows-v2-release\SimRtProbe.exe dist\v2-release\bin\SimRtProbe.exe >nul
exit /b %errorlevel%
