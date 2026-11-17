@echo off
setlocal
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
cmake -S . -B build\windows-v2-release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_DEPENDS_USE_COMPILER=FALSE -DCMAKE_PREFIX_PATH="%QT_PREFIX%" -DSIM_WITH_QT=ON -DSIM_BUILD_TESTS=ON
if errorlevel 1 exit /b 1
cmake --build build\windows-v2-release
if errorlevel 1 exit /b 1
ctest --test-dir build\windows-v2-release --output-on-failure
exit /b %errorlevel%
