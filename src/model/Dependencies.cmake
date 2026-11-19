find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets Xml)
string(TOLOWER "${CMAKE_BUILD_TYPE}" model_configuration)
set(model_sdks "${PROJECT_SOURCE_DIR}/build/model-deps")
find_package(VTK 9.4.2 EXACT CONFIG REQUIRED COMPONENTS GUISupportQt RenderingOpenGL2
    InteractionStyle InteractionWidgets RenderingAnnotation RenderingFreeType IOXML IOLegacy
    HINTS "${model_sdks}/sdk-${model_configuration}/lib/cmake/vtk-9.4")
find_package(HDF5 1.14.6 EXACT CONFIG REQUIRED COMPONENTS C shared
    HINTS "${model_sdks}/hdf-sdk/HDF5-1.14.6-win64/cmake")

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
