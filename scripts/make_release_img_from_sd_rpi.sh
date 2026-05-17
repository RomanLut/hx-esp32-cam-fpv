#!/usr/bin/env bash
set -euo pipefail

SD_DEVICE="/dev/mmcblk0"
USB_MOUNT="/mnt/usb1"
IMAGE_PATH="$USB_MOUNT/espvrx_rpi.img"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GS_DIR="$REPO_ROOT/gs"
PISHRINK_URL="https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/release/scripts/pishrink.sh"
PISHRINK_PATH="/usr/local/bin/pishrink.sh"

MOUNTED_BY_SCRIPT=false

#===================================================================================
#===================================================================================
# Selects the available USB partition used as the release image target.
select_usb_device()
{
    if mountpoint -q "$USB_MOUNT"; then
        MOUNT_SOURCE="$(findmnt -n -o SOURCE --target "$USB_MOUNT")"
        case "$MOUNT_SOURCE" in
            /dev/sda1|/dev/sdb1)
                USB_DEVICE="$MOUNT_SOURCE"
                return 0
                ;;
        esac

        echo "USB mount point is already mounted from $MOUNT_SOURCE, expected /dev/sda1 or /dev/sdb1" >&2
        exit 1
    fi

    for CANDIDATE in /dev/sda1 /dev/sdb1; do
        if [ -b "$CANDIDATE" ]; then
            USB_DEVICE="$CANDIDATE"
            return 0
        fi
    done

    echo "USB flash device is not available: expected /dev/sda1 or /dev/sdb1" >&2
    exit 1
}

#===================================================================================
#===================================================================================
# Unmounts the USB drive when this script mounted it and exits before completion.
cleanup()
{
    if [ "$MOUNTED_BY_SCRIPT" = true ] && mountpoint -q "$USB_MOUNT"; then
        sudo umount "$USB_MOUNT"
    fi
}

#===================================================================================
#===================================================================================
# Removes files that are useful while building the image but should not be shipped.
cleanup_release_files()
{
    echo "Disk usage before release cleanup:"
    df -h /

    rm -f "$GS_DIR"/*.avi
    rm -rf "$GS_DIR/build"

    rm -rf "$REPO_ROOT/OpenCV/OpenCVWrapper/Build"
    rm -rf "$REPO_ROOT/.cache"
    rm -rf "$HOME/.cache/pip"
    rm -rf "$HOME/.cache/cmake"
    rm -rf "$HOME/.ccache"

    rm -rf "$HOME/SDL2-2.0.18/build"
    rm -rf "$REPO_ROOT/OpenCV/OpenCV/doc"
    rm -rf "$REPO_ROOT/OpenCV/OpenCV/samples"
    rm -rf "$REPO_ROOT/OpenCV/OpenCV/data"

    sudo apt-get clean
    sudo rm -rf /var/cache/apt/archives/*.deb
    sudo rm -rf /var/lib/apt/lists/*
    sudo journalctl --vacuum-time=1d >/dev/null 2>&1 || true
    sudo rm -rf /tmp/* /var/tmp/*

    echo "Disk usage after release cleanup:"
    df -h /
}

trap cleanup EXIT

if [ ! -d "$GS_DIR" ]; then
    echo "GS directory is not available: $GS_DIR" >&2
    exit 1
fi

if [ ! -b "$SD_DEVICE" ]; then
    echo "SD card device is not available: $SD_DEVICE" >&2
    exit 1
fi

cd "$GS_DIR"
cleanup_release_files

wget -O pishrink.sh "$PISHRINK_URL"
sudo chmod +x pishrink.sh
sudo mv pishrink.sh "$PISHRINK_PATH"

sudo mkdir -p "$USB_MOUNT"
select_usb_device

if mountpoint -q "$USB_MOUNT"; then
    MOUNT_SOURCE="$(findmnt -n -o SOURCE --target "$USB_MOUNT")"
    if [ "$MOUNT_SOURCE" != "$USB_DEVICE" ]; then
        echo "USB mount point is already mounted from $MOUNT_SOURCE, expected $USB_DEVICE" >&2
        exit 1
    fi
else
    sudo mount "$USB_DEVICE" "$USB_MOUNT"
    MOUNTED_BY_SCRIPT=true
fi

if ! mountpoint -q "$USB_MOUNT"; then
    echo "USB mount point is not available: $USB_MOUNT" >&2
    exit 1
fi

if ! sudo test -w "$USB_MOUNT"; then
    echo "USB mount point is not writable: $USB_MOUNT" >&2
    exit 1
fi

sudo dd if="$SD_DEVICE" of="$IMAGE_PATH" bs=1M status=progress
sudo "$PISHRINK_PATH" -z -a "$IMAGE_PATH"

sudo umount "$USB_MOUNT"
MOUNTED_BY_SCRIPT=false
