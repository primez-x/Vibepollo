cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories(
        "${CMAKE_SOURCE_DIR}"
)

set(TOOL_SOURCES
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/logging_policy.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
)

add_executable(dxgi-info dxgi.cpp ${TOOL_SOURCES})
target_link_libraries(dxgi-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES}
)
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp ${TOOL_SOURCES})
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# One-shot COM boundary for default render endpoint policy. This executable
# is deliberately separate from the Sunshine service and is installed under
# the tools directory next to the service binary.
add_executable(sunshine_audio_policy_helper
        audio_policy_helper.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/audio_policy_helper_protocol.cpp)
set_target_properties(sunshine_audio_policy_helper PROPERTIES CXX_STANDARD 23)
target_link_libraries(sunshine_audio_policy_helper
        ole32
        mmdevapi
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_audio_policy_helper PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(atmos-capability-probe
        atmos_capability_probe.cpp
        atmos_capability_probe_app.cpp
        atmos_capability_probe_cli.cpp
        atmos_capability_probe_policy.cpp
        atmos_capability_probe_windows.cpp)
target_link_libraries(atmos-capability-probe
        nlohmann_json::nlohmann_json
        ole32
        windowsapp
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(atmos-capability-probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(atmos-pcm-spatial-probe
        atmos_pcm_spatial_probe.cpp
        atmos_pcm_spatial_probe_main.cpp
        atmos_pcm_spatial_probe_windows.cpp)
target_link_libraries(atmos-pcm-spatial-probe
        nlohmann_json::nlohmann_json
        ole32
        mmdevapi
        ${PLATFORM_LIBRARIES}
)
target_compile_options(atmos-pcm-spatial-probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Read-only discovery for NVIDIA's installed signed virtual-audio capture ABI.
# The probe calls only the public factory export and never registers an endpoint
# or starts capture; the production integration can gate on the exact returned
# interface version before invoking any session method.
add_executable(nvidia-nvaudcap-probe
        nvidia_nvaudcap_probe.cpp
        nvidia_nvaudcap_probe_main.cpp
        nvidia_nvaudcap_probe_windows.cpp)
target_link_libraries(nvidia-nvaudcap-probe
        nlohmann_json::nlohmann_json
        version
        wintrust
        ${PLATFORM_LIBRARIES})
target_compile_options(nvidia-nvaudcap-probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(atmos-spatial-lock-test
        atmos_spatial_lock_test.cpp
        atmos_spatial_lock_test_windows.cpp)
target_link_libraries(atmos-spatial-lock-test
        nlohmann_json::nlohmann_json
        ole32
        windowsapp
        ksuser
        mmdevapi
        ${PLATFORM_LIBRARIES}
)
target_compile_options(atmos-spatial-lock-test PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Opaque Dolby MAT direct-relay building blocks.  These deliberately remain
# separate from the legacy Opus path: the host module consumes the driver's
# fixed record ABI and the client module writes the unchanged carrier through
# exclusive WASAPI.
add_library(atmos_mat_direct_protocol STATIC
        atmos_mat_direct_protocol.cpp)
target_compile_options(atmos_mat_direct_protocol PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_library(atmos_mat_host_relay STATIC
        atmos_mat_host_relay.cpp
        atmos_mat_host_relay_windows.cpp)
target_link_libraries(atmos_mat_host_relay
        PUBLIC atmos_mat_direct_protocol
        PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(atmos_mat_host_relay PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_library(atmos_mat_client_writer STATIC
        atmos_mat_client_writer.cpp
        atmos_mat_client_writer_windows.cpp
        atmos_capability_probe_policy.cpp
        atmos_capability_probe_windows.cpp)
target_link_libraries(atmos_mat_client_writer
        PRIVATE ole32 windowsapp ksuser mmdevapi ${PLATFORM_LIBRARIES})
target_compile_options(atmos_mat_client_writer PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_library(atmos_mat_direct_relay_cli STATIC atmos_mat_direct_relay_cli.cpp)
target_link_libraries(atmos_mat_direct_relay_cli
        PUBLIC atmos_mat_host_relay atmos_mat_client_writer
        PRIVATE OpenSSL::SSL OpenSSL::Crypto ws2_32)
target_compile_options(atmos_mat_direct_relay_cli PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(atmos_mat_direct_relay_host atmos_mat_direct_relay_host_main.cpp)
target_link_libraries(atmos_mat_direct_relay_host PRIVATE atmos_mat_direct_relay_cli)
target_compile_options(atmos_mat_direct_relay_host PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(atmos_mat_direct_relay_client atmos_mat_direct_relay_client_main.cpp)
target_link_libraries(atmos_mat_direct_relay_client PRIVATE atmos_mat_direct_relay_cli)
target_compile_options(atmos_mat_direct_relay_client PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Native, fixed-path one-shot deployer for the two-file audio hotfix. Keep it
# independent of the Sunshine runtime so recovery does not depend on the files
# that it is replacing.
add_executable(vibepollo_audio_hotfix_deployer
        vibepollo_audio_hotfix_deployer.cpp
        vibepollo_audio_hotfix_deployer_core.cpp)
set_target_properties(vibepollo_audio_hotfix_deployer PROPERTIES
        CXX_STANDARD 23
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(vibepollo_audio_hotfix_deployer
        advapi32
        iphlpapi
        ws2_32)
target_compile_options(vibepollo_audio_hotfix_deployer PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
if(MINGW)
    target_link_options(vibepollo_audio_hotfix_deployer PRIVATE
            -municode
            -static
            -static-libgcc
            -static-libstdc++)
endif()
add_custom_command(TARGET vibepollo_audio_hotfix_deployer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:vibepollo_audio_hotfix_deployer>" "${CMAKE_BINARY_DIR}/wix_payload/tools/vibepollo_audio_hotfix_deployer.exe")

if(BUILD_TESTS)
    add_executable(vibepollo_audio_hotfix_deployer_test_cli EXCLUDE_FROM_ALL
            vibepollo_audio_hotfix_deployer.cpp
            vibepollo_audio_hotfix_deployer_core.cpp)
    set_target_properties(vibepollo_audio_hotfix_deployer_test_cli PROPERTIES
            CXX_STANDARD 23
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(vibepollo_audio_hotfix_deployer_test_cli PRIVATE
            VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING=1)
    target_link_libraries(vibepollo_audio_hotfix_deployer_test_cli
            advapi32
            iphlpapi
            ws2_32)
    target_compile_options(vibepollo_audio_hotfix_deployer_test_cli PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
    if(MINGW)
        target_link_options(vibepollo_audio_hotfix_deployer_test_cli PRIVATE
                -municode
                -static
                -static-libgcc
                -static-libstdc++)
    endif()
endif()

add_executable(sunshinesvc sunshinesvc.cpp)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_custom_command(TARGET sunshinesvc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:sunshinesvc>" "${CMAKE_BINARY_DIR}/wix_payload/tools/sunshinesvc.exe"
)

# Playnite launcher: per-launch helper that hosts a private IPC server
add_executable(playnite-launcher
        playnite_launcher/main.cpp
        playnite_launcher/launcher.cpp
        playnite_launcher/arguments.cpp
        playnite_launcher/cleanup.cpp
        playnite_launcher/focus_utils.cpp
        playnite_launcher/lossless_scaling.cpp
        playnite_launcher/lossless_scaling_policy.cpp
        playnite_launcher/playnite_process.cpp)
set_target_properties(playnite-launcher PROPERTIES CXX_STANDARD 23)
set_target_properties(playnite-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(playnite-launcher PRIVATE ${CMAKE_SOURCE_DIR}/tools/playnite_launcher)
target_sources(playnite-launcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_ipc.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp)
target_compile_definitions(playnite-launcher PRIVATE SUNSHINE_PLAYNITE_LAUNCHER)
target_link_libraries(playnite-launcher
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        nlohmann_json::nlohmann_json
        user32
        userenv
        ole32
        oleaut32
        Psapi
        shell32
        shlwapi
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(playnite-launcher PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# Add sunshine_wgc_capture executable
add_executable(sunshine_wgc_capture
        sunshine_wgc_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp)
set_target_properties(sunshine_wgc_capture PROPERTIES CXX_STANDARD 23)

# Define helper build flag to conditionally compile logging
target_compile_definitions(sunshine_wgc_capture PRIVATE
        SUNSHINE_EXTERNAL_PROCESS=1
)

target_link_libraries(sunshine_wgc_capture
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        d3d11
        dxgi
        ole32
        user32
        gdi32
        windowsapp
        avrt
        shcore
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_wgc_capture PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Display settings helper: detached helper that applies/reverts display settings via IPC.
# One exe hosts both engines (legacy + v2), selected by --engine= or persisted state.
add_executable(sunshine_display_helper
        display_settings_helper_main.cpp
        display_settings_helper.cpp
        display_settings_helper_v2.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/diagnostics.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/diagnostics_boost.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/file_text_storage.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/topology_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/async_dispatcher.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/golden_health.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/operations.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/snapshot.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/snapshot_codec.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/snapshot_file_storage.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/state_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_display_settings.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_event_pump.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_platform_workarounds.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_shell_refresh_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_scheduled_task_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp)
set_target_properties(sunshine_display_helper PROPERTIES CXX_STANDARD 23)
target_compile_definitions(sunshine_display_helper PRIVATE SETUP_LIBDISPLAYDEVICE_LOGGING)
target_include_directories(sunshine_display_helper PRIVATE ${CMAKE_SOURCE_DIR}/third-party)
target_link_libraries(sunshine_display_helper
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        libdisplaydevice::display_device
        user32
        wtsapi32
        advapi32
        secur32
        ole32
        taskschd
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_display_helper PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
