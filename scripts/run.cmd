@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
if /I "%~1"=="Release" goto release
set "BUILD_DIR=build\windows-debug"
if exist "%BUILD_DIR%\SimTransfer.exe" goto launch
echo ERROR: "%BUILD_DIR%\SimTransfer.exe" was not found. Run scripts\build.cmd first.
exit /b 1

:release
set "BUILD_DIR=build\windows-release"
if not exist "build\windows-release\CMakeCache.txt" if exist "build\windows-v2-release\CMakeCache.txt" set "BUILD_DIR=build\windows-v2-release"
cmake --preset windows-release -B "%BUILD_DIR%" -DCMAKE_PREFIX_PATH="%QT_PREFIX%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --target SimTransfer
if errorlevel 1 exit /b 1

:launch
echo Launching "%CD%\%BUILD_DIR%\SimTransfer.exe"
"%BUILD_DIR%\SimTransfer.exe"
exit /b %ERRORLEVEL%
