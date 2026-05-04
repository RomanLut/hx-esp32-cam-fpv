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

# Minimal Ubuntu WSL images often lack cmake. When OPENCV_WRAPPER_BOOTSTRAP_CMAKE is unset or 1,
# download a standalone Kitware CMake tarball to /tmp (no sudo). Set OPENCV_WRAPPER_BOOTSTRAP_CMAKE=0 to fail fast with apt hints instead.
ensure_cmake_on_path()
{
    if command -v cmake >/dev/null 2>&1
    then
        return 0
    fi

    if [[ "${OPENCV_WRAPPER_BOOTSTRAP_CMAKE:-1}" == "0" ]]
    then
        echo "cmake was not found in PATH. Install with: sudo apt-get update && sudo apt-get install -y cmake build-essential" >&2
        echo "Or do not set OPENCV_WRAPPER_BOOTSTRAP_CMAKE=0 so this script can download portable CMake to /tmp." >&2
        exit 1
    fi

    if ! command -v curl >/dev/null 2>&1
    then
        echo "cmake was not found and curl is missing. Install: sudo apt-get update && sudo apt-get install -y curl ca-certificates cmake build-essential" >&2
        exit 1
    fi

    local uname_m arch cmake_ver cmake_dir tarname url
    uname_m="$(uname -m)"
    cmake_ver="3.29.6"
    case "${uname_m}" in
        x86_64)
            arch="x86_64"
            ;;
        aarch64|arm64)
            arch="aarch64"
            ;;
        *)
            echo "cmake was not found; portable bootstrap only supports x86_64 or aarch64 (uname -m=${uname_m}). Install cmake from your distro." >&2
            exit 1
            ;;
    esac

    cmake_dir="/tmp/cmake-${cmake_ver}-linux-${arch}"
    tarname="cmake-${cmake_ver}-linux-${arch}.tar.gz"
    url="https://github.com/Kitware/CMake/releases/download/v${cmake_ver}/${tarname}"

    if [[ ! -x "${cmake_dir}/bin/cmake" ]]
    then
        echo "cmake not found; downloading portable CMake ${cmake_ver} for linux-${arch} to ${cmake_dir} ..."
        rm -rf "${cmake_dir}" "/tmp/${tarname}"
        curl -L --fail --show-error -o "/tmp/${tarname}" "${url}"
        tar -xzf "/tmp/${tarname}" -C /tmp
    fi

    export PATH="${cmake_dir}/bin:${PATH}"
    if ! command -v cmake >/dev/null 2>&1
    then
        echo "Portable CMake bootstrap failed (expected ${cmake_dir}/bin/cmake)." >&2
        exit 1
    fi
    echo "Using portable CMake: $(command -v cmake) ($("${cmake_dir}/bin/cmake" --version | head -n 1))"
}

ensure_cmake_on_path

echo "Building OpenCVWrapper Linux prebuilt with ${BUILD_JOBS} parallel jobs."

cmake -S "${OPENCV_SOURCE}" -B "${OPENCV_BUILD}" \
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

cmake --build "${WRAPPER_BUILD}" --config "${CONFIG}" --parallel "${BUILD_JOBS}"
cmake --install "${WRAPPER_BUILD}" --config "${CONFIG}" --strip
