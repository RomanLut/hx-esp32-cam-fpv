#!/usr/bin/env bash
set -euo pipefail

SD_DEVICE="/dev/mmcblk1"
USB_MOUNT="/mnt/usb1"
IMAGE_PATH="$USB_MOUNT/espvrx_dualboot_radxa3w.img"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GS_DIR="$REPO_ROOT/gs"
ZERO_SCRIPT_URL="https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/release/scripts/zero_free_space.sh"

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

trap cleanup EXIT

if [ ! -b "$SD_DEVICE" ]; then
    echo "SD card device is not available: $SD_DEVICE" >&2
    exit 1
fi

sudo timedatectl set-ntp true

if [ -d "$GS_DIR" ]; then
    cd "$GS_DIR"
    rm -f ./*.avi
else
    echo "GS directory is not available: $GS_DIR" >&2
    exit 1
fi

cd "$GS_DIR"
wget -O zero_free_space.sh "$ZERO_SCRIPT_URL"
sudo chmod +x zero_free_space.sh
./zero_free_space.sh

sudo mkdir -p "$USB_MOUNT"
select_usb_device

if ! mountpoint -q "$USB_MOUNT"; then
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

sudo rm -f /etc/growroot-grown
sudo rm -f /etc/resize2fs-done
rm -f /home/radxa/ruby/config/boot_count.cfg

sudo dd if="$SD_DEVICE" of="$IMAGE_PATH" bs=1M status=progress

sudo umount "$USB_MOUNT"
MOUNTED_BY_SCRIPT=false
