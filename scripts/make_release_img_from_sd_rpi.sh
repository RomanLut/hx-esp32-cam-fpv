#!/usr/bin/env bash
set -euo pipefail

USB_DEVICE="/dev/sdb1"
SD_DEVICE="/dev/mmcblk0"
USB_MOUNT="/mnt/usb1"
IMAGE_PATH="$USB_MOUNT/espvrx_rpi.img"
GS_DIR="$HOME/esp32-cam-fpv/gs"
PISHRINK_URL="https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/release/scripts/pishrink.sh"
PISHRINK_PATH="/usr/local/bin/pishrink.sh"

MOUNTED_BY_SCRIPT=false

#===================================================================================
#===================================================================================
# Unmounts the USB drive when this script mounted it and exits before completion.
cleanup()
{
    if [ "$MOUNTED_BY_SCRIPT" = true ] && mountpoint -q "$USB_MOUNT"; then
        sudo umount "$USB_MOUNT"
    fi
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

if [ ! -b "$USB_DEVICE" ]; then
    echo "USB flash device is not available: $USB_DEVICE" >&2
    exit 1
fi

cd "$GS_DIR"
rm -f ./*.avi

wget -O pishrink.sh "$PISHRINK_URL"
sudo chmod +x pishrink.sh
sudo mv pishrink.sh "$PISHRINK_PATH"

sudo mkdir -p "$USB_MOUNT"

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
