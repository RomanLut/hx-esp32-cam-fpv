#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WRAPPER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCV_SOURCE="${OPENCV_SOURCE:-${WRAPPER_ROOT}/../OpenCV}"
BUILD_ROOT="${BUILD_ROOT:-${WRAPPER_ROOT}/Build/android-${ANDROID_ABI:-arm64-v8a}}"
CONFIG="${CONFIG:-Release}"
EXPECTED_OPENCV_TAG="${EXPECTED_OPENCV_TAG:-4.13.0}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_MIN_SDK="${ANDROID_MIN_SDK:-23}"
PREBUILT_PLATFORM="${PREBUILT_PLATFORM:-android/${ANDROID_ABI}}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

if [[ -z "${ANDROID_NDK_HOME:-}" && -z "${ANDROID_NDK_ROOT:-}" ]]; then
    if [[ -n "${ANDROID_SDK_ROOT:-}" && -d "${ANDROID_SDK_ROOT}/ndk" ]]; then
        ANDROID_NDK_HOME="$(find "${ANDROID_SDK_ROOT}/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
    fi
else
    ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT}}"
fi

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "Android NDK was not found. Set ANDROID_NDK_HOME or ANDROID_SDK_ROOT." >&2
    exit 1
fi

TOOLCHAIN="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"
if [[ ! -f "${TOOLCHAIN}" ]]; then
    echo "Android toolchain file was not found at ${TOOLCHAIN}." >&2
    exit 1
fi

if [[ -z "${CMAKE_EXE:-}" ]]; then
    if [[ -n "${ANDROID_SDK_ROOT:-}" && -d "${ANDROID_SDK_ROOT}/cmake" ]]; then
        CMAKE_DIR="$(find "${ANDROID_SDK_ROOT}/cmake" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
        CMAKE_EXE="${CMAKE_DIR}/bin/cmake"
    else
        CMAKE_EXE="cmake"
    fi
fi

if [[ -z "${NINJA_EXE:-}" ]]; then
    CMAKE_BIN_DIR="$(dirname "${CMAKE_EXE}")"
    if [[ -x "${CMAKE_BIN_DIR}/ninja" ]]; then
        NINJA_EXE="${CMAKE_BIN_DIR}/ninja"
    else
        NINJA_EXE="ninja"
    fi
fi

OPENCV_BUILD="${BUILD_ROOT}/opencv"
OPENCV_INSTALL="${BUILD_ROOT}/opencv-install"
WRAPPER_BUILD="${BUILD_ROOT}/wrapper"

OPENCV_TAG=""
if OPENCV_TAG="$(git -C "${OPENCV_SOURCE}" describe --tags --exact-match 2>/dev/null)"; then
    :
elif [[ -f "${WRAPPER_ROOT}/OPENCV_VERSION.txt" ]]; then
    OPENCV_TAG="$(sed -n 's/^OpenCV tag:[[:space:]]*//p' "${WRAPPER_ROOT}/OPENCV_VERSION.txt" | head -n 1)"
fi
if [[ "${OPENCV_TAG}" != "${EXPECTED_OPENCV_TAG}" ]]; then
    echo "OpenCV source must be pinned to tag ${EXPECTED_OPENCV_TAG}, but found '${OPENCV_TAG}'." >&2
    exit 1
fi

echo "Building Android OpenCVWrapper for ${ANDROID_ABI} with NDK ${ANDROID_NDK_HOME} and ${BUILD_JOBS} parallel jobs."

"${CMAKE_EXE}" -S "${OPENCV_SOURCE}" -B "${OPENCV_BUILD}" \
    -G Ninja -DCMAKE_MAKE_PROGRAM="${NINJA_EXE}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DANDROID_ABI="${ANDROID_ABI}" \
    -DANDROID_PLATFORM="android-${ANDROID_MIN_SDK}" \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DCMAKE_INSTALL_PREFIX="${OPENCV_INSTALL}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_LIST=core,imgproc,calib3d,video \
    -DCPU_DISPATCH= \
    -DBUILD_TESTS=OFF \
    -DBUILD_PERF_TESTS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_opencv_apps=OFF \
    -DBUILD_ANDROID_PROJECTS=OFF \
    -DBUILD_ANDROID_EXAMPLES=OFF \
    -DBUILD_PROTOBUF=OFF \
    -DBUILD_JAVA=OFF \
    -DBUILD_opencv_python2=OFF \
    -DBUILD_opencv_python3=OFF \
    -DWITH_AVIF=OFF \
    -DWITH_IPP=OFF \
    -DWITH_JASPER=OFF \
    -DWITH_JPEG=OFF \
    -DWITH_OPENCL=OFF \
    -DWITH_OPENEXR=OFF \
    -DWITH_OPENJPEG=OFF \
    -DWITH_PNG=OFF \
    -DWITH_PROTOBUF=OFF \
    -DWITH_QT=OFF \
    -DWITH_TIFF=OFF \
    -DWITH_WEBP=OFF \
    -DWITH_ZLIB=OFF

"${CMAKE_EXE}" --build "${OPENCV_BUILD}" --config "${CONFIG}" --target install --parallel "${BUILD_JOBS}"

OPENCV_CONFIG="${OPENCV_BUILD}/OpenCVConfig.cmake"
if [[ ! -f "${OPENCV_CONFIG}" ]]; then
    echo "OpenCVConfig.cmake was not found under ${OPENCV_BUILD}." >&2
    exit 1
fi

"${CMAKE_EXE}" -S "${WRAPPER_ROOT}" -B "${WRAPPER_BUILD}" \
    -G Ninja -DCMAKE_MAKE_PROGRAM="${NINJA_EXE}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DANDROID_ABI="${ANDROID_ABI}" \
    -DANDROID_PLATFORM="android-${ANDROID_MIN_SDK}" \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DOpenCV_DIR="${OPENCV_BUILD}" \
    -DOPENCV_WRAPPER_PREBUILT_PLATFORM="${PREBUILT_PLATFORM}"

"${CMAKE_EXE}" --build "${WRAPPER_BUILD}" --config "${CONFIG}" --target install --parallel "${BUILD_JOBS}"
