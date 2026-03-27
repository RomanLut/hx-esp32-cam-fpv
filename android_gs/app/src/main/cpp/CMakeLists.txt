cmake_minimum_required(VERSION 3.22.1)
project(android_gs_core)

include(ExternalProject)

set(LIBJPEG_TURBO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/libjpeg-turbo)
set(LIBJPEG_TURBO_BINARY_DIR ${CMAKE_BINARY_DIR}/libjpeg-turbo)
set(LIBJPEG_TURBO_INSTALL_DIR ${CMAKE_BINARY_DIR}/libjpeg-turbo-install)
set(LIBJPEG_TURBO_LIBRARY ${LIBJPEG_TURBO_INSTALL_DIR}/lib/libturbojpeg.a)
file(MAKE_DIRECTORY ${LIBJPEG_TURBO_BINARY_DIR})
file(MAKE_DIRECTORY ${LIBJPEG_TURBO_INSTALL_DIR}/include)
file(MAKE_DIRECTORY ${LIBJPEG_TURBO_INSTALL_DIR}/lib)

ExternalProject_Add(libjpeg_turbo_ext
    SOURCE_DIR ${LIBJPEG_TURBO_SOURCE_DIR}
    BINARY_DIR ${LIBJPEG_TURBO_BINARY_DIR}
    INSTALL_DIR ${LIBJPEG_TURBO_INSTALL_DIR}
    CMAKE_GENERATOR ${CMAKE_GENERATOR}
    BUILD_BYPRODUCTS ${LIBJPEG_TURBO_LIBRARY}
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DANDROID_NDK=${ANDROID_NDK}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${LIBJPEG_TURBO_INSTALL_DIR}
        -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE
        -DENABLE_SHARED=FALSE
        -DENABLE_STATIC=TRUE
        -DWITH_TURBOJPEG=TRUE
        -DWITH_JAVA=FALSE
        -DWITH_TESTS=FALSE
        -DWITH_TOOLS=FALSE
)

add_library(turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(turbojpeg PROPERTIES
    IMPORTED_LOCATION ${LIBJPEG_TURBO_LIBRARY}
)
add_dependencies(turbojpeg libjpeg_turbo_ext)

add_library(android_gs_core SHARED
    android_jpeg_decoder.cpp
    android_video_renderer.cpp
    native_bridge.cpp
    ../../../../../gs/src/imgui/imgui.cpp
    ../../../../../gs/src/imgui/imgui_draw.cpp
    ../../../../../gs/src/imgui/imgui_tables.cpp
    ../../../../../gs/src/imgui/imgui_widgets.cpp
    ../../../../../gs/src/imgui/backends/imgui_impl_opengl3.cpp
    ../../../../../gs/src/utils/lodepng.cpp
    ../../../../../gs/src/core/gs_protocol.cpp
    ../../../../../gs/src/core/gs_session_core.cpp
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

target_include_directories(android_gs_core PRIVATE
    ../../../../../gs/src
    ../../../../../gs/src/utils
    ../../../../../gs/src/imgui
    ../../../../../gs/src/imgui/backends
    ../../../../../components/common
    ${LIBJPEG_TURBO_INSTALL_DIR}/include
)

target_compile_definitions(android_gs_core PRIVATE IMGUI_IMPL_OPENGL_ES2)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(android_gs_core
    turbojpeg
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
    ${log-lib}
)
