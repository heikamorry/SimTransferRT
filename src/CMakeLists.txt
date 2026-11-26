add_library(sim_rt_core STATIC
    protocol/Checksum.cpp protocol/PacketCodec.cpp
    core/Types.cpp core/SamplingClock.cpp
)
target_include_directories(sim_rt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sim_rt_core PRIVATE SIM_BUILD_TYPE="$<CONFIG>")
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE SIM_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" RESULT_VARIABLE SIM_GIT_DIRTY ERROR_QUIET)
target_compile_definitions(sim_rt_core PRIVATE SIM_SOURCE_REV="${SIM_GIT_REV}" SIM_SOURCE_DIRTY="${SIM_GIT_DIRTY}")
target_link_libraries(sim_rt_core PUBLIC Threads::Threads)
set_target_properties(sim_rt_core PROPERTIES AUTOMOC OFF AUTORCC OFF)

add_library(sim_rt_v2 STATIC
    protocol/V2PacketCodec.cpp sources/AuroraCodec.cpp sources/AuroraSource.cpp sources/NdiSource.cpp
    v2/Config.cpp v2/Engine.cpp v2/ProducerWait.cpp
    v2/Runner.cpp v2/RunnerReport.cpp v2/Receiver.cpp support/V2CliOptions.cpp
)
target_link_libraries(sim_rt_v2 PUBLIC sim_rt_core)
target_compile_definitions(sim_rt_v2 PUBLIC SIM_PRODUCT_VERSION="${PROJECT_VERSION}")
target_compile_definitions(sim_rt_v2 PRIVATE SIM_BUILD_TYPE="$<CONFIG>" SIM_SOURCE_REV="${SIM_GIT_REV}" SIM_SOURCE_DIRTY="${SIM_GIT_DIRTY}")
set_target_properties(sim_rt_v2 PROPERTIES AUTOMOC OFF AUTORCC OFF)
if(WIN32)
    target_link_libraries(sim_rt_v2 PRIVATE iphlpapi ws2_32)
endif()

add_library(sim_rt_native STATIC network/NativeUdpTransport.cpp)
target_link_libraries(sim_rt_native PUBLIC sim_rt_core)
set_target_properties(sim_rt_native PROPERTIES AUTOMOC OFF AUTORCC OFF)
if(WIN32)
    target_link_libraries(sim_rt_native PRIVATE ws2_32)
endif()

if(SIM_WITH_ASIO)
    add_library(sim_rt_asio STATIC network/AsioUdpTransport.cpp network/AsioReceiverSocket.cpp)
    target_link_libraries(sim_rt_asio PUBLIC sim_rt_core sim_asio_headers)
    target_compile_definitions(sim_rt_asio PUBLIC SIM_WITH_ASIO=1)
    set_target_properties(sim_rt_asio PROPERTIES AUTOMOC OFF AUTORCC OFF)
    if(WIN32)
        target_link_libraries(sim_rt_asio PRIVATE ws2_32 mswsock)
    endif()
endif()

add_executable(SimRtProbe native_cli.cpp support/V2Cli.cpp)
target_link_libraries(SimRtProbe PRIVATE sim_rt_native sim_rt_v2)
if(WIN32)
    set(product_version_rc "${CMAKE_CURRENT_BINARY_DIR}/product_version.rc")
    configure_file(../resources/product_version.rc.in "${product_version_rc}" @ONLY)
    target_sources(SimRtProbe PRIVATE "${product_version_rc}")
endif()
if(SIM_WITH_ASIO)
    target_link_libraries(SimRtProbe PRIVATE sim_rt_asio)
endif()
set_target_properties(SimRtProbe PROPERTIES AUTOMOC OFF AUTORCC OFF)

if(SIM_WITH_QT)
    qt_add_executable(SimTransfer
        main.cpp ui/V2Window.cpp ui/V2Window.h ../resources/resources.qrc
    )
    if(WIN32)
        # Explorer/taskbar icon; the .ico itself is embedded through the Qt resource too.
        target_sources(SimTransfer PRIVATE ../resources/app.rc "${product_version_rc}")
        target_include_directories(SimTransfer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../resources)
    endif()
    target_link_libraries(SimTransfer PRIVATE sim_rt_v2 sim_rt_asio Qt6::Widgets)
    if(SIM_MODEL_PREVIEW)
        add_subdirectory(model)
        target_link_libraries(SimTransfer PRIVATE sim_model_ui)
        sim_stage_model_runtime(SimTransfer)
    endif()
    set_target_properties(SimTransfer PROPERTIES WIN32_EXECUTABLE TRUE)

    qt_add_executable(SimTransferCli cli_main.cpp support/V2Cli.cpp)
    target_link_libraries(SimTransferCli PRIVATE sim_rt_v2 sim_rt_native sim_rt_asio Qt6::Core)
    if(WIN32)
        target_sources(SimTransferCli PRIVATE ../resources/app.rc "${product_version_rc}")
        target_include_directories(SimTransferCli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../resources)
    endif()
    set_target_properties(SimTransferCli PROPERTIES AUTOMOC OFF AUTORCC OFF)

    install(TARGETS SimTransfer SimTransferCli SimRtProbe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(WIN32 AND SIM_DEPLOY_QT)
        qt_generate_deploy_app_script(TARGET SimTransfer OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
        install(SCRIPT ${deploy_script})
    endif()
endif()
