@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
set "PRESET=windows-debug"
if /I "%~1"=="Release" set "PRESET=windows-release"
cmake --preset "%PRESET%" -DCMAKE_PREFIX_PATH="%QT_PREFIX%"
if errorlevel 1 exit /b 1
cmake --build --preset "%PRESET%"
if errorlevel 1 exit /b 1
ctest --preset "%PRESET%"
if errorlevel 1 exit /b 1
echo BUILD AND TESTS PASSED: %PRESET%
exit /b 0
