#!/usr/bin/env bash
set -euo pipefail

# Function to determine the number of make jobs based on free memory
get_make_jobs() {
    local threshold="$1"
    local low_jobs="$2"
    local high_jobs="$3"
    
    FREE_MEMORY=$(free -m | awk '/^Mem:/{print $4}')
    if [ "$FREE_MEMORY" -lt "$threshold" ]; then
        echo "$low_jobs"
    else
        echo "$high_jobs"
    fi
}

version_ge() {
    [ "$1" = "$2" ] && return 0
    [ "$(printf "%s\n%s\n" "$1" "$2" | sort -V | tail -n 1)" = "$1" ]
}

ensure_legacy_ruby_apt_sources() {
    if ! grep -q "raspbian.raspberrypi.org/raspbian/ buster" /etc/apt/sources.list 2>/dev/null; then
        return
    fi

    echo "Detected legacy RubyFPV buster apt source. Switching to Debian archive mirrors."

    cat <<'EOF' | sudo tee /etc/apt/sources.list > /dev/null
deb [trusted=yes] http://archive.debian.org/debian buster main contrib non-free
deb [trusted=yes] http://archive.debian.org/debian-security buster/updates main contrib non-free
EOF

    cat <<'EOF' | sudo tee /etc/apt/apt.conf.d/99archive-no-check-valid-until > /dev/null
Acquire::Check-Valid-Until false;
EOF
}

cmake_satisfies_min_version() {
    local required_version="3.18.0"
    local current_version=""

    if ! command -v cmake >/dev/null 2>&1; then
        return 1
    fi

    current_version=$(cmake --version | head -n 1 | awk '{print $3}')
    version_ge "$current_version" "$required_version"
}

sudo timedatectl set-ntp true
ensure_legacy_ruby_apt_sources

IS_RADXA=false

COMPATIBLE_FILE="/proc/device-tree/compatible"

if [ -f "$COMPATIBLE_FILE" ]; then
    COMPATIBLE_CONTENT=$(tr -d '\000' < "$COMPATIBLE_FILE")

    # Check if the content contains "radxa,zero3"
    if echo "$COMPATIBLE_CONTENT" | grep -q "radxa,zero3"; then
        IS_RADXA=true
    fi
fi

if [ "$IS_RADXA" = true ]; then
    HOME_DIRECTORY="/home/radxa"
    MAKE_JOBS=$(get_make_jobs 512 1 4)
else
    HOME_DIRECTORY="/home/pi"
    MAKE_JOBS=1
fi

echo "HOME_DIRECTORY=$HOME_DIRECTORY"
echo "MAKE_JOBS=$MAKE_JOBS"

sudo apt update

sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms git aircrack-ng cmake

if [ "$IS_RADXA" = true ]; then
    echo "Skipping SDL recompilation for Radxa."
else
    cd "$HOME_DIRECTORY"
    wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz
    tar zxf SDL2-2.0.18.tar.gz
    rm SDL2-2.0.18.tar.gz

    cd SDL2-2.0.18
    ./autogen.sh
    ./configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl
    
    make -j"$MAKE_JOBS"
    sudo make install
fi

cd "$HOME_DIRECTORY"
if [ ! -d esp32-cam-fpv/.git ]; then
    git clone -b release --recursive --shallow-submodules https://github.com/RomanLut/esp32-cam-fpv
fi
cd esp32-cam-fpv

if [ -f OpenCV/OpenCVWrapper/scripts/build_linux.sh ]; then
    BUILD_WRAPPER_SCRIPT="OpenCV/OpenCVWrapper/scripts/build_linux.sh"
elif [ -f OpenCVWrapper/scripts/build_linux.sh ]; then
    BUILD_WRAPPER_SCRIPT="OpenCVWrapper/scripts/build_linux.sh"
else
    echo "ERROR: OpenCV wrapper build script not found."
    exit 1
fi

TOTAL_MEM_MB=$(free -m | awk '/^Mem:/{print $2}')

if ! cmake_satisfies_min_version; then
    echo "Skipping OpenCV wrapper build: cmake >= 3.18.0 is required."
elif [ "$TOTAL_MEM_MB" -lt 512 ]; then
    echo "Skipping OpenCV wrapper build: total memory is ${TOTAL_MEM_MB} MB (< 512 MB)."
else
    BUILD_JOBS="$MAKE_JOBS" bash "$BUILD_WRAPPER_SCRIPT"
fi

cd gs
make -j"$MAKE_JOBS"

PROFILE_FILE="/root/.profile"

sudo sed -i \
    -e 's|^\./ruby_start|#&|' \
    -e 's|^echo "Launch done."|#&|' \
    "$PROFILE_FILE"

echo "$HOME_DIRECTORY/esp32-cam-fpv/scripts/boot_selection.sh" | sudo tee -a "$PROFILE_FILE"

sudo reboot
