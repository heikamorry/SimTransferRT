@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0.."
call scripts\env.cmd
if errorlevel 1 exit /b 1
set "CFG=Release"
set "SUFFIX=release"
if /i "%~1"=="Debug" (
    set "CFG=Debug"
    set "SUFFIX=debug"
)
set "DEP=build/model-deps"
set "VSLANG=1033"
cmake -S "%DEP%/VTK-9.4.2" -B "%DEP%/native-%SUFFIX%" -G Ninja ^
 -DCMAKE_BUILD_TYPE=%CFG% "-DCMAKE_INSTALL_PREFIX=%CD%/%DEP%/sdk-%SUFFIX%" "-DCMAKE_PREFIX_PATH=%QT_PREFIX%" ^
 -DBUILD_SHARED_LIBS=ON -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF -DVTK_BUILD_ALL_MODULES=OFF ^
 -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT -DVTK_GROUP_ENABLE_Rendering=DONT_WANT -DVTK_GROUP_ENABLE_Imaging=DONT_WANT ^
 -DVTK_GROUP_ENABLE_MPI=NO -DVTK_GROUP_ENABLE_Qt=DONT_WANT -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES ^
 -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES ^
 -DVTK_MODULE_ENABLE_VTK_IOXML=YES -DVTK_MODULE_ENABLE_VTK_IOLegacy=YES -DVTK_MODULE_ENABLE_VTK_hdf5=NO ^
 -DVTK_QT_VERSION=6 -DVTK_SMP_IMPLEMENTATION_TYPE=Sequential -DVTK_ENABLE_WRAPPING=OFF
if errorlevel 1 exit /b 1
cmake --build "%DEP%/native-%SUFFIX%" --parallel 10
if errorlevel 1 exit /b 1
cmake --install "%DEP%/native-%SUFFIX%"
exit /b %errorlevel%
