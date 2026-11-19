@echo off
rem Intentionally no SETLOCAL: caller needs the activated environment.
if not defined QT_PREFIX set "QT_PREFIX=C:\Qt\6.12.0\msvc2022_64"
if not defined VS_BUILD_TOOLS set "VS_BUILD_TOOLS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
if not exist "%VS_BUILD_TOOLS%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: set VS_BUILD_TOOLS to your Build Tools directory.
    exit /b 1
)
call "%VS_BUILD_TOOLS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44
if errorlevel 1 exit /b 1
if not exist "%QT_PREFIX%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo ERROR: Qt6Config.cmake missing. Set QT_PREFIX to your Qt MSVC SDK.
    exit /b 1
)
set "PATH=%QT_PREFIX%\bin;%PATH%"
echo Qt SDK: %QT_PREFIX%
echo MSVC toolset: %VCToolsVersion%
exit /b 0
