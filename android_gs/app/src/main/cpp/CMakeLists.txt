cmake_minimum_required(VERSION 3.22.1)
project(android_gs_core)

add_library(android_gs_core SHARED
    android_apfpv_transport.cpp
    android_raw_broadcast_transport.cpp
    android_surface_backend.cpp
    android_bitmap_jpeg_decoder.cpp
    android_osd_font_storage.cpp
    android_playback_manager.cpp
    android_recordings_storage.cpp
    android_runtime_platform_services.cpp
    android_transport_manager.cpp
    android_wifi_scan_transport.cpp
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
    ../../../../../components_gs/shared/gs_lens_correction_shared.cpp
    ../../../../../components_gs/shared/gs_video_layout_shared.cpp
    ../../../../../components_gs/shared/gs_video_shader_renderer.cpp
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
    third_party/devourer/hal/Hal8812PwrSeq.c
    third_party/devourer/hal/hal8812a_fw.c
    third_party/devourer/src/Radiotap.c
    third_party/devourer/src/EepromManager.cpp
    third_party/devourer/src/FirmwareManager.cpp
    third_party/devourer/src/FrameParser.cpp
    third_party/devourer/src/HalModule.cpp
    third_party/devourer/src/ParsedRadioPacket.cpp
    third_party/devourer/src/RadioManagementModule.cpp
    third_party/devourer/src/Rtl8812aDevice.cpp
    third_party/devourer/src/RtlUsbAdapter.cpp
    third_party/devourer/src/WiFiDriver.cpp
)

set_source_files_properties(
    android_bitmap_jpeg_decoder.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math;-funroll-loops"
)

target_include_directories(android_gs_core PRIVATE
    .
    third_party/include
    third_party/devourer/src
    third_party/devourer/hal
    ../../../../../components_gs
    ../../../../../components_gs/shared
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

target_compile_definitions(android_gs_core PRIVATE IMGUI_IMPL_OPENGL_ES2)

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
