add_library(sim_rt_core STATIC
    protocol/Checksum.cpp protocol/PacketCodec.cpp
    core/Types.cpp core/LogicalFrameBuilder.cpp core/RealtimeStream.cpp core/SamplingClock.cpp
    application/CommandDispatcher.cpp application/StreamRunner.cpp application/AcceptanceReport.cpp support/CliOptions.cpp
)
target_include_directories(sim_rt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sim_rt_core PRIVATE SIM_BUILD_TYPE="$<CONFIG>")
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE SIM_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" RESULT_VARIABLE SIM_GIT_DIRTY ERROR_QUIET)
target_compile_definitions(sim_rt_core PRIVATE SIM_SOURCE_REV="${SIM_GIT_REV}" SIM_SOURCE_DIRTY="${SIM_GIT_DIRTY}")
target_link_libraries(sim_rt_core PUBLIC Threads::Threads)
set_target_properties(sim_rt_core PROPERTIES AUTOMOC OFF AUTORCC OFF)
add_library(sim_rt_native STATIC network/NativeUdpTransport.cpp)
target_link_libraries(sim_rt_native PUBLIC sim_rt_core)
set_target_properties(sim_rt_native PROPERTIES AUTOMOC OFF AUTORCC OFF)
if(WIN32)
    target_link_libraries(sim_rt_native PRIVATE ws2_32)
endif()
add_executable(SimRtProbe native_cli.cpp)
target_link_libraries(SimRtProbe PRIVATE sim_rt_native)
set_target_properties(SimRtProbe PROPERTIES AUTOMOC OFF AUTORCC OFF)
if(SIM_WITH_QT)
    add_library(sim_rt_app STATIC
        network/QtUdpTransport.cpp
        application/SessionManager.cpp application/SessionManager.h
        application/TransferController.cpp application/TransferController.h
    )
    target_link_libraries(sim_rt_app PUBLIC sim_rt_core Qt6::Core Qt6::Network)
    if(WIN32)
        target_link_libraries(sim_rt_app PRIVATE ws2_32)
    endif()
    qt_add_executable(SimTransfer
        main.cpp ui/MainWindow.cpp ui/MainWindow.h ui/ConfigPanel.cpp ui/ConfigPanel.h
        ui/FrameTableModel.cpp ui/FrameTableModel.h ../resources/resources.qrc
    )
    target_link_libraries(SimTransfer PRIVATE sim_rt_app Qt6::Widgets)
    set_target_properties(SimTransfer PROPERTIES WIN32_EXECUTABLE TRUE)
    qt_add_executable(SimTransferCli cli_main.cpp)
    target_link_libraries(SimTransferCli PRIVATE sim_rt_app)
    install(TARGETS SimTransfer SimTransferCli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(WIN32 AND SIM_DEPLOY_QT)
        qt_generate_deploy_app_script(TARGET SimTransfer OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
        install(SCRIPT ${deploy_script})
    endif()
endif()
