cmake_minimum_required(VERSION 3.22.1)
project(android_gs_core)

add_library(android_gs_core SHARED
    native_bridge.cpp
    ../../../../../gs/src/core/gs_protocol.cpp
    ../../../../../gs/src/core/gs_session_core.cpp
    ../../../../../gs/src/core/osd_menu_common.cpp
    ../../../../../gs/src/core/video_frame_assembler.cpp
    ../../../../../components/common/crc.cpp
    ../../../../../components/common/fec.cpp
    ../../../../../components/common/packet_filter.cpp
    ../../../../../components/common/wifi_channels.cpp
)

target_include_directories(android_gs_core PRIVATE
    ../../../../../gs/src
    ../../../../../components/common
)

find_library(log-lib log)

target_link_libraries(android_gs_core
    ${log-lib}
)
