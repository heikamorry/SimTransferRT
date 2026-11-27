find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets Xml)
string(TOLOWER "${CMAKE_BUILD_TYPE}" model_configuration)
if(WIN32)
    set(model_sdks "${PROJECT_SOURCE_DIR}/build/model-deps")
    set(vtk_hints HINTS "${model_sdks}/sdk-${model_configuration}/lib/cmake/vtk-9.4")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_language(C)
endif()
find_package(VTK 9.4.2 EXACT CONFIG REQUIRED COMPONENTS GUISupportQt RenderingOpenGL2
    InteractionStyle InteractionWidgets RenderingAnnotation RenderingFreeType IOXML IOLegacy
    ${vtk_hints})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ZLIB REQUIRED)
    find_package(HDF5 1.14.6 EXACT CONFIG REQUIRED COMPONENTS C static)
    set(sim_hdf5_library ${HDF5_C_STATIC_LIBRARY})
    if(NOT TARGET "${sim_hdf5_library}" OR NOT HDF5_ENABLE_Z_LIB_SUPPORT)
        message(FATAL_ERROR "The Linux SDK must provide the static HDF5 C library with DEFLATE.")
    endif()
    get_target_property(glad_include_dirs VTK::glad INTERFACE_INCLUDE_DIRECTORIES)
    find_file(glad_config vtk_glad.h PATHS ${glad_include_dirs} NO_DEFAULT_PATH NO_CACHE REQUIRED)
    file(STRINGS "${glad_config}" sim_vtk_gles REGEX "^#define VTK_MODULE_vtkglad_GLES3 +1$")
else()
    find_package(HDF5 1.14.6 EXACT CONFIG REQUIRED COMPONENTS C shared
        HINTS "${model_sdks}/hdf-sdk/HDF5-1.14.6-win64/cmake")
    set(sim_hdf5_library hdf5::hdf5-shared)
endif()

# A relocated Qt6Core searches next to the EXE. Stage the QPA plugins for the
# build tree as well as the installer, including the matching Debug variants.
function(sim_stage_model_runtime target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
            "-Ddirectory=$<TARGET_FILE_DIR:${target}>"
            "-Dlibraries=$<TARGET_RUNTIME_DLLS:${target}>"
            "-Dplatforms=$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>;$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageRuntime.cmake" VERBATIM)
    endif()
endfunction()
