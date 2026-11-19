add_library(sim_model_io STATIC Model.cpp Ply.cpp Vtp.cpp Legacy.cpp Archive.cpp Conversion.cpp)
target_link_libraries(sim_model_io PUBLIC Qt6::Core sim_rt_v2 PRIVATE Qt6::Xml VTK::IOCore hdf5::hdf5-shared)
add_library(sim_model_ui STATIC PreviewPanel.cpp PreviewPanel.h Scene.cpp)
target_link_libraries(sim_model_ui PUBLIC sim_model_io Qt6::Widgets Qt6::OpenGLWidgets
    VTK::GUISupportQt VTK::RenderingOpenGL2 VTK::RenderingFreeType
    VTK::RenderingAnnotation VTK::InteractionWidgets VTK::InteractionStyle)
target_compile_definitions(sim_model_ui PUBLIC SIM_MODEL_PREVIEW=1)
vtk_module_autoinit(TARGETS sim_model_ui MODULES VTK::RenderingOpenGL2 VTK::RenderingFreeType VTK::InteractionStyle)
if(WIN32)
    get_filename_component(model_vtk_bin "${VTK_DIR}/../../../bin" ABSOLUTE)
    install(DIRECTORY "${model_vtk_bin}/" DESTINATION bin FILES_MATCHING PATTERN "vtk*.dll")
    install(FILES "$<TARGET_FILE:hdf5::hdf5-shared>" DESTINATION bin)
    install(DIRECTORY "${VTK_DIR}/../../../share/licenses/" DESTINATION share/licenses OPTIONAL)
    install(FILES "${HDF5_DIR}/../COPYING" "${HDF5_DIR}/../COPYING_LBNL_HDF5"
        DESTINATION share/licenses/HDF5 OPTIONAL)
endif()
