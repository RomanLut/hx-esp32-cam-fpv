cmake_minimum_required(VERSION 3.18)

project(OpenCVWrapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENCV_WRAPPER_BUILD_SHARED "Build the OpenCV wrapper as a shared library" ON)

if(OPENCV_WRAPPER_BUILD_SHARED)
    add_library(OpenCVWrapper SHARED
        src/CameraCalibration.cpp
    )
else()
    add_library(OpenCVWrapper STATIC
        src/CameraCalibration.cpp
    )
endif()

add_library(OpenCVWrapper::OpenCVWrapper ALIAS OpenCVWrapper)

find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d)

target_include_directories(OpenCVWrapper
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(OpenCVWrapper
    PRIVATE
        ${OpenCV_LIBS}
)

target_compile_definitions(OpenCVWrapper
    PRIVATE
        OPENCV_WRAPPER_BUILDING_LIBRARY
)

if(MSVC)
    target_compile_options(OpenCVWrapper PRIVATE /W4)
else()
    target_compile_options(OpenCVWrapper PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(OPENCV_WRAPPER_PREBUILT_PLATFORM "" CACHE STRING "Prebuilt output platform name")

if(OPENCV_WRAPPER_PREBUILT_PLATFORM STREQUAL "")
    if(ANDROID)
        set(OPENCV_WRAPPER_PREBUILT_PLATFORM "android/${ANDROID_ABI}")
    elseif(WIN32)
        set(OPENCV_WRAPPER_PREBUILT_PLATFORM "windows/x64")
    elseif(APPLE)
        set(OPENCV_WRAPPER_PREBUILT_PLATFORM "macos/universal")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(OPENCV_WRAPPER_PREBUILT_PLATFORM "linux/arm64")
    else()
        set(OPENCV_WRAPPER_PREBUILT_PLATFORM "linux/x64")
    endif()
endif()

set(OPENCV_WRAPPER_PREBUILT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/Prebuilt/${OPENCV_WRAPPER_PREBUILT_PLATFORM}"
    CACHE PATH "Directory where built wrapper artifacts are installed"
)

install(TARGETS OpenCVWrapper
    RUNTIME DESTINATION "${OPENCV_WRAPPER_PREBUILT_DIR}"
    LIBRARY DESTINATION "${OPENCV_WRAPPER_PREBUILT_DIR}"
    ARCHIVE DESTINATION "${OPENCV_WRAPPER_PREBUILT_DIR}"
)

install(DIRECTORY include/
    DESTINATION "${OPENCV_WRAPPER_PREBUILT_DIR}/include"
)
