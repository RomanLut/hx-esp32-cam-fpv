cmake_minimum_required(VERSION 3.22.1)
project(android_gs_core)

add_library(android_gs_core SHARED
    android_surface_backend.cpp
    android_bitmap_jpeg_decoder.cpp
    android_asset_flight_osd.cpp
    native_bridge.cpp
    ../../../../../gs/src/gs_runtime_aux_flow.cpp
    ../../../../../gs/src/gs_runtime_config.cpp
    ../../../../../gs/src/gs_runtime_core.cpp
    ../../../../../gs/src/gs_runtime_event_classify.cpp
    ../../../../../gs/src/gs_runtime_event_pipeline.cpp
    ../../../../../gs/src/gs_runtime_event_dispatch.cpp
    ../../../../../gs/src/gs_runtime_event_flow.cpp
    ../../../../../gs/src/gs_runtime_frame_ui.cpp
    ../../../../../gs/src/gs_runtime_globals.cpp
    ../../../../../gs/src/gs_runtime_menu_ui.cpp
    ../../../../../gs/src/gs_runtime_overlay_state.cpp
    ../../../../../gs/src/gs_runtime_packet_flow.cpp
    ../../../../../gs/src/gs_runtime_protocol.cpp
    ../../../../../gs/src/gs_runtime_session.cpp
    ../../../../../gs/src/gs_runtime_state.cpp
    ../../../../../gs/src/gs_runtime_sync.cpp
    ../../../../../gs/src/gs_runtime_video_flow.cpp
    ../../../../../gs/src/gs_osd_font_shared.cpp
    ../../../../../gs/src/gs_storage_status_shared.cpp
    ../../../../../gs/src/settings_storage.cpp
    ../../../../../gs/src/fontwalksnail.cpp
    ../../../../../gs/src/gs_asset_flight_osd.cpp
    ../../../../../gs/src/gs_imgui_overlay_shared.cpp
    ../../../../../gs/src/gs_top_overlay_shared.cpp
    ../../../../../gs/src/gs_flight_osd_render_shared.cpp
    ../../../../../gs/src/gs_video_layout_shared.cpp
    ../../../../../gs/src/gs_video_renderer.cpp
    ../../../../../gs/src/imgui/imgui.cpp
    ../../../../../gs/src/imgui/imgui_draw.cpp
    ../../../../../gs/src/imgui/imgui_tables.cpp
    ../../../../../gs/src/imgui/imgui_widgets.cpp
    ../../../../../gs/src/imgui/backends/imgui_impl_opengl3.cpp
    ../../../../../gs/src/utils/lodepng.cpp
    ../../../../../gs/src/core/gs_protocol.cpp
    ../../../../../gs/src/core/gs_session_core.cpp
    ../../../../../gs/src/core/osd_base.cpp
    ../../../../../gs/src/core/frame_packets_debug_core.cpp
    ../../../../../gs/src/core/osd_menu_common.cpp
    ../../../../../gs/src/core/osd_menu_controller.cpp
    ../../../../../gs/src/core/osd_menu_imgui_shared.cpp
    ../../../../../gs/src/core/stats_panel_shared.cpp
    ../../../../../gs/src/core/video_frame_assembler.cpp
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
    ../../../../../gs/src
    ../../../../../gs/src/utils
    ../../../../../gs/src/imgui
    ../../../../../gs/src/imgui/backends
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
