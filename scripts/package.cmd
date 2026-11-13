@echo off
setlocal
cd /d "%~dp0.."
call scripts\build.cmd Release
if errorlevel 1 exit /b 1
call scripts\env.cmd
if errorlevel 1 exit /b 1
cmake --install build\windows-release --prefix "%CD%\dist"
if errorlevel 1 exit /b 1
echo Test dist\bin\SimTransfer.exe on a Windows computer without Qt installed.
exit /b 0
