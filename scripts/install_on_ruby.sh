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

sudo timedatectl set-ntp true

IS_RADXA=false

COMPATIBLE_FILE="/proc/device-tree/compatible"

if [ -f "$COMPATIBLE_FILE" ]; then
    COMPATIBLE_CONTENT=$(cat "$COMPATIBLE_FILE")

    # Check if the content contains "radxa,zero3"
    if echo "$COMPATIBLE_CONTENT" | grep -q "radxa,zero3w"; then
        IS_RADXA=true
    fi
fi

if [ "$IS_RADXA" = true ]; then
    HOME_DIRECTORY="/home/radxa"
else
    HOME_DIRECTORY="/home/pi"
fi

echo "HOME_DIRECTORY=$HOME_DIRECTORY"

sudo apt update

sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms git aircrack-ng

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
    
    MAKE_JOBS=$(get_make_jobs 512 2 4)
    echo "MAKE_JOBS=$MAKE_JOBS"

    make -j"$MAKE_JOBS"
    sudo make install
fi

cd "$HOME_DIRECTORY"
git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv
cd esp32-cam-fpv
cd gs

MAKE_JOBS=$(get_make_jobs 512 1 4)
echo "MAKE_JOBS=$MAKE_JOBS"

make -j"$MAKE_JOBS"

PROFILE_FILE="/root/.profile"

sudo sed -i \
    -e 's|^\./ruby_start|#&|' \
    -e 's|^echo "Launch done."|#&|' \
    "$PROFILE_FILE"

echo "$HOME_DIRECTORY/esp32-cam-fpv/scripts/boot_selection.sh" | sudo tee -a "$PROFILE_FILE"

sudo reboot
