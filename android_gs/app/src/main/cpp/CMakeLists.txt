cmake_minimum_required(VERSION 3.22.1)
project(android_gs_core)

add_library(android_gs_core SHARED
    android_surface_backend.cpp
    android_bitmap_jpeg_decoder.cpp
    android_asset_flight_osd.cpp
    android_osd_font_storage.cpp
    native_bridge.cpp
    ../../../../../components_gs/shared/gs_runtime_aux_flow.cpp
    ../../../../../components_gs/shared/gs_runtime_config.cpp
    ../../../../../components_gs/shared/gs_runtime_core.cpp
    ../../../../../components_gs/shared/gs_runtime_event_classify.cpp
    ../../../../../components_gs/shared/gs_runtime_event_pipeline.cpp
    ../../../../../components_gs/shared/gs_runtime_event_dispatch.cpp
    ../../../../../components_gs/shared/gs_runtime_event_flow.cpp
    ../../../../../components_gs/shared/gs_runtime_frame_ui.cpp
    ../../../../../components_gs/shared/gs_runtime_globals.cpp
    ../../../../../components_gs/shared/gs_runtime_menu_ui.cpp
    ../../../../../components_gs/shared/gs_runtime_overlay_state.cpp
    ../../../../../components_gs/shared/gs_runtime_packet_flow.cpp
    ../../../../../components_gs/shared/gs_runtime_protocol.cpp
    ../../../../../components_gs/shared/gs_runtime_session.cpp
    ../../../../../components_gs/shared/gs_runtime_state.cpp
    ../../../../../components_gs/shared/gs_runtime_sync.cpp
    ../../../../../components_gs/shared/gs_runtime_video_flow.cpp
    ../../../../../components_gs/shared/gs_runtime_osd_font_storage.cpp
    ../../../../../components_gs/shared/gs_osd_font_shared.cpp
    ../../../../../components_gs/shared/gs_storage_status_shared.cpp
    ../../../../../components_gs/shared/settings_storage.cpp
    ../../../../../components_gs/shared/fontwalksnail.cpp
    ../../../../../components_gs/shared/gs_asset_flight_osd.cpp
    ../../../../../components_gs/shared/gs_imgui_overlay_shared.cpp
    ../../../../../components_gs/shared/gs_top_overlay_shared.cpp
    ../../../../../components_gs/shared/gs_flight_osd_render_shared.cpp
    ../../../../../components_gs/shared/gs_video_layout_shared.cpp
    ../../../../../components_gs/shared/gs_video_renderer.cpp
    ../../../../../components_gs/imgui/imgui.cpp
    ../../../../../components_gs/imgui/imgui_draw.cpp
    ../../../../../components_gs/imgui/imgui_tables.cpp
    ../../../../../components_gs/imgui/imgui_widgets.cpp
    ../../../../../components_gs/imgui/backends/imgui_impl_opengl3.cpp
    ../../../../../components_gs/utils/lodepng.cpp
    ../../../../../components_gs/shared/core/gs_protocol.cpp
    ../../../../../components_gs/shared/core/gs_session_core.cpp
    ../../../../../components_gs/shared/core/osd_base.cpp
    ../../../../../components_gs/shared/core/frame_packets_debug_core.cpp
    ../../../../../components_gs/shared/core/osd_menu_common.cpp
    ../../../../../components_gs/shared/core/osd_menu_controller.cpp
    ../../../../../components_gs/shared/core/osd_menu_imgui_shared.cpp
    ../../../../../components_gs/shared/core/stats_panel_shared.cpp
    ../../../../../components_gs/shared/core/video_frame_assembler.cpp
    ../../../../../components/common/crc.cpp
    ../../../../../components/common/fec.cpp
    ../../../../../components/common/fec_block_decoder.cpp
    ../../../../../components/common/packet_filter.cpp
    ../../../../../components/common/util.cpp
    ../../../../../components/common/wifi_channels.cpp
)

set_source_files_properties(
    android_bitmap_jpeg_decoder.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math;-funroll-loops"
)

target_include_directories(android_gs_core PRIVATE
    .
    ../../../../../components_gs
    ../../../../../components_gs/shared
    ../../../../../components_gs/utils
    ../../../../../components_gs/imgui
    ../../../../../components_gs/imgui/backends
    ../../../../../components/common
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
    ${log-lib}
)
