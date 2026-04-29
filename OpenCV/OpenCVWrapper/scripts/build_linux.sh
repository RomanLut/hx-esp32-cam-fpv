#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WRAPPER_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCV_SOURCE="${OPENCV_SOURCE:-${WRAPPER_ROOT}/../OpenCV}"
BUILD_ROOT="${BUILD_ROOT:-${WRAPPER_ROOT}/Build/linux-$(uname -m)}"
CONFIG="${CONFIG:-Release}"
EXPECTED_OPENCV_TAG="${EXPECTED_OPENCV_TAG:-4.13.0}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

case "$(uname -m)" in
    aarch64|arm64)
        PREBUILT_PLATFORM="${PREBUILT_PLATFORM:-linux/arm64}"
        ;;
    *)
        PREBUILT_PLATFORM="${PREBUILT_PLATFORM:-linux/x64}"
        ;;
esac

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

echo "Building OpenCVWrapper Linux prebuilt with ${BUILD_JOBS} parallel jobs."

cmake -S "${OPENCV_SOURCE}" -B "${OPENCV_BUILD}" \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DCMAKE_INSTALL_PREFIX="${OPENCV_INSTALL}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_LIST=core,imgproc,calib3d \
    -DCPU_DISPATCH= \
    -DBUILD_TESTS=OFF \
    -DBUILD_PERF_TESTS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_opencv_apps=OFF \
    -DBUILD_PROTOBUF=OFF \
    -DBUILD_JAVA=OFF \
    -DBUILD_opencv_python2=OFF \
    -DBUILD_opencv_python3=OFF \
    -DWITH_1394=OFF \
    -DWITH_AVIF=OFF \
    -DWITH_FFMPEG=OFF \
    -DWITH_GSTREAMER=OFF \
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

cmake --build "${OPENCV_BUILD}" --config "${CONFIG}" --target install --parallel "${BUILD_JOBS}"

OPENCV_CONFIG="${OPENCV_BUILD}/OpenCVConfig.cmake"
if [[ ! -f "${OPENCV_CONFIG}" ]]; then
    echo "OpenCVConfig.cmake was not found under ${OPENCV_BUILD}." >&2
    exit 1
fi

cmake -S "${WRAPPER_ROOT}" -B "${WRAPPER_BUILD}" \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DOpenCV_DIR="${OPENCV_BUILD}" \
    -DOPENCV_WRAPPER_PREBUILT_PLATFORM="${PREBUILT_PLATFORM}"

cmake --build "${WRAPPER_BUILD}" --config "${CONFIG}" --target install --parallel "${BUILD_JOBS}"
