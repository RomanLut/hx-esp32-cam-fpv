cmake_minimum_required(VERSION 3.22.1)
project(android_gs_core)

set(ANDROID_SHARED_DEVOURER_DIR ../../../../../components_gs/android_shared/devourer)

add_library(android_gs_core SHARED
    ../../../../../components_gs/android_shared/cpp/android_apfpv_transport.cpp
    ../../../../../components_gs/android_shared/cpp/android_raw_broadcast_transport.cpp
    android_surface_backend.cpp
    ../../../../../components_gs/android_shared/cpp/android_bitmap_jpeg_decoder.cpp
    ../../../../../components_gs/android_shared/cpp/android_osd_font_storage.cpp
    ../../../../../components_gs/android_shared/cpp/android_playback_manager.cpp
    ../../../../../components_gs/android_shared/cpp/android_recordings_storage.cpp
    ../../../../../components_gs/android_shared/cpp/android_runtime_platform_services.cpp
    ../../../../../components_gs/android_shared/cpp/android_serial_telemetry.cpp
    ../../../../../components_gs/android_shared/cpp/android_transport_manager.cpp
    ../../../../../components_gs/android_shared/cpp/android_wifi_scan_transport.cpp
    native_bridge.cpp
    ../../../../../components_gs/mcp/gs_mcp_server.cpp
    ../../../../../components_gs/shared/frame_packets_debug.cpp
    ../../../../../components_gs/shared/gs_runtime_config.cpp
    ../../../../../components_gs/shared/gs_runtime_core.cpp
    ../../../../../components_gs/shared/gs_runtime_event_pipeline.cpp
    ../../../../../components_gs/shared/gs_runtime_event_flow.cpp
    ../../../../../components_gs/shared/gs_runtime_globals.cpp
    ../../../../../components_gs/shared/gs_runtime_menu_ui.cpp
    ../../../../../components_gs/shared/gs_runtime_protocol.cpp
    ../../../../../components_gs/shared/gs_runtime_state.cpp
    ../../../../../components_gs/shared/gs_runtime_sync.cpp
    ../../../../../components_gs/shared/gs_runtime_video_flow.cpp
    ../../../../../components_gs/shared/gs_recordings_storage.cpp
    ../../../../../components_gs/shared/gs_playback_manager.cpp
    ../../../../../components_gs/shared/gs_asset_loader.cpp
    ../../../../../components_gs/shared/gs_stub_transport.cpp
    ../../../../../components_gs/shared/gs_test_transport.cpp
    ../../../../../components_gs/shared/gs_wifi_scan_transport.cpp
    ../../../../../components_gs/shared/settings_storage.cpp
    ../../../../../components_gs/shared/fontwalksnail.cpp
    ../../../../../components_gs/shared/flight_osd.cpp
    ../../../../../components_gs/shared/gs_top_overlay_shared.cpp
    ../../../../../components_gs/shared/gs_camera_calibration_shared.cpp
    ../../../../../components_gs/shared/gs_video_stabilization_shared.cpp
    ../../../../../components_gs/shared/gs_lens_correction_shared.cpp
    ../../../../../components_gs/shared/gs_video_layout_shared.cpp
    ../../../../../components_gs/shared/gs_video_shader_renderer.cpp
    ../../../../../components_gs/shared/gs_jpeg_dct_postprocessing.cpp
    ../../../../../components_gs/shared/gs_video_renderer.cpp
    ../../../../../components_gs/imgui/imgui.cpp
    ../../../../../components_gs/imgui/imgui_draw.cpp
    ../../../../../components_gs/imgui/imgui_tables.cpp
    ../../../../../components_gs/imgui/imgui_widgets.cpp
    ../../../../../components_gs/imgui/backends/imgui_impl_opengl3.cpp
    ../../../../../components_gs/fmt/format.cc
    ../../../../../components_gs/fmt/os.cc
    ../../../../../components_gs/utils/lodepng.cpp
    ../../../../../components_gs/utils/utils.cpp
    ../../../../../components_gs/shared/core/gs_protocol.cpp
    ../../../../../components_gs/shared/core/gs_session_core.cpp
    ../../../../../components_gs/shared/core/transport_manager_base.cpp
    ../../../../../components_gs/shared/core/transport_globals.cpp
    ../../../../../components_gs/shared/core/transport_kind.cpp
    ../../../../../components_gs/shared/core/osd_menu_common.cpp
    ../../../../../components_gs/shared/core/osd_menu_controller.cpp
    ../../../../../components_gs/shared/core/osd_menu_imgui_shared.cpp
    ../../../../../components_gs/shared/core/stats_panel_shared.cpp
    ../../../../../components_gs/shared/core/video_frame_assembler.cpp
    ../../../../../components/common/crc.cpp
    ../../../../../components/common/avi.cpp
    ../../../../../components/common/fec.cpp
    ../../../../../components/common/fec_block_decoder.cpp
    ../../../../../components/common/jpeg_parser.cpp
    ../../../../../components/common/packet_filter.cpp
    ../../../../../components/common/packets.cpp
    ../../../../../components/common/util.cpp
    ../../../../../components/common/wifi_channels.cpp
    ../../../../../components/common/hx_mavlink_parser.cpp
    # --- devourer core (generation-agnostic) ---
    ${ANDROID_SHARED_DEVOURER_DIR}/src/Event.h
    ${ANDROID_SHARED_DEVOURER_DIR}/src/ParsedRadioPacket.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/Radiotap.c
    ${ANDROID_SHARED_DEVOURER_DIR}/src/RadiotapBuilder.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/RtlAdapter.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/UsbTransport.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/UsbDeviceLock.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/UsbOpen.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/TxMode.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/TxPower.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/LinkHealth.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/SignalStop.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/WiFiDriver.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/PhyTableLoader.cpp

    # --- Jaguar1: RTL8812AU / 8811AU / 8821AU / 8814AU ---
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/Hal8812PwrSeq.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/Hal8821APwrSeq.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/Hal8812a_TxPwrTrack.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8812a_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8821a_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/RtlJaguarDevice.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/HalModule.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/RadioManagementModule.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/EepromManager.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/FirmwareManager.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/Iqk8812a.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/PhydmWatchdog.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/PowerTracking8812a.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/FrameParser.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/BbDbgportReader.cpp

    # RTL8814AU (4T4R)
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/Hal8814PwrSeq.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8814a_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8814a/Hal8814_PhyTables.c
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar1/Iqk8814a.cpp

    # --- Jaguar2: RTL8822BU / RTL8811CU / RTL8821CU ---
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/RtlJaguar2Device.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/HalJaguar2.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/HalmacJaguar2Fw.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/HalmacJaguar2MacInit.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/Phy8822bTables.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/Halrf8822b.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8822b_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822b/Hal8822b_PhyTables.c
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/Phy8821cTables.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar2/Halrf8821c.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8821c_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8821c/Hal8821c_PhyTables.c

    # --- Jaguar3: RTL8812CU / RTL8822CU / RTL8812EU / RTL8822EU ---
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/RtlJaguar3Device.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/HalJaguar3.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/HalmacJaguar3Fw.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/HalmacJaguar3MacInit.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/RadioManagementJaguar3.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/PhyTableLoaderJaguar3.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/Phy8822cTables.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/Halrf8822c.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8822c_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822c/Hal8822c_PhyTables.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822c/Hal8822c_IqkNctl.c
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/Phy8822eTables.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src/jaguar3/Halrf8822e.cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/hal8822e_fw.c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822e/Hal8822e_PhyTables.c
)

target_compile_definitions(android_gs_core PRIVATE
    DEVOURER_HAVE_JAGUAR1=1
    DEVOURER_HAVE_8814=1
    DEVOURER_HAVE_JAGUAR2=1
    DEVOURER_HAVE_JAGUAR2_8822B=1
    DEVOURER_HAVE_JAGUAR2_8821C=1
    DEVOURER_HAVE_JAGUAR3=1
    DEVOURER_HAVE_JAGUAR3_8822C=1
    DEVOURER_HAVE_JAGUAR3_8822E=1
)

set_source_files_properties(
    ../../../../../components_gs/android_shared/cpp/android_bitmap_jpeg_decoder.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math;-funroll-loops"
)

target_include_directories(android_gs_core PRIVATE
    .
    third_party/include
    ../../../../../components_gs
    ../../../../../components_gs/shared
    ../../../../../components_gs/android_shared
    ../../../../../components_gs/android_shared/cpp
    ${ANDROID_SHARED_DEVOURER_DIR}/src
    ${ANDROID_SHARED_DEVOURER_DIR}/hal
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8814a
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8821c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822b
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822c
    ${ANDROID_SHARED_DEVOURER_DIR}/hal/phydm/rtl8822e
    ../../../../../components_gs/utils
    ../../../../../components_gs/imgui
    ../../../../../components_gs/imgui/backends
    ../../../../../components/common
)

add_library(libusb_prebuilt SHARED IMPORTED)
set_target_properties(libusb_prebuilt PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/prebuilt/${ANDROID_ABI}/libusb1.0.so
)

add_library(opencv_wrapper_prebuilt SHARED IMPORTED)
set_target_properties(opencv_wrapper_prebuilt PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../OpenCV/OpenCVWrapper/Prebuilt/android/${ANDROID_ABI}/libOpenCVWrapper.so
)

target_compile_definitions(android_gs_core PRIVATE
    IMGUI_IMPL_OPENGL_ES2
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(android_gs_core
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
    libusb_prebuilt
    opencv_wrapper_prebuilt
    ${log-lib}
)
