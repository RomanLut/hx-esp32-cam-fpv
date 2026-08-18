#!/usr/bin/env bash
set -euo pipefail

SD_DEVICE="/dev/mmcblk0"
USB_MOUNT="/mnt/usb1"
IMAGE_NAME="espvrx_rpi.img"
IMAGE_PATH="$USB_MOUNT/$IMAGE_NAME"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GS_DIR="$REPO_ROOT/gs"
PISHRINK_URL="https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/release/scripts/pishrink.sh"
PISHRINK_PATH="/usr/local/bin/pishrink.sh"

MOUNTED_BY_SCRIPT=false
USB_DEVICE=""
USB_DISK=""

#===================================================================================
#===================================================================================
# Rejects configured Git credential helpers and credentials embedded in remote URLs.
verify_no_git_credentials()
{
    local CREDENTIAL_HELPERS
    local REMOTE_OUTPUT

    if ! git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "Repository is not a Git work tree: $REPO_ROOT" >&2
        exit 1
    fi

    CREDENTIAL_HELPERS="$(git -C "$REPO_ROOT" config --show-origin --get-all credential.helper 2>/dev/null || true)"
    if [ -n "$CREDENTIAL_HELPERS" ]; then
        echo "Credential test failed: Git credential.helper is configured:" >&2
        printf '%s\n' "$CREDENTIAL_HELPERS" >&2
        exit 1
    fi

    REMOTE_OUTPUT="$(git -C "$REPO_ROOT" remote -v)"
    if printf '%s\n' "$REMOTE_OUTPUT" | grep -Eiq 'https?://[^/@[:space:]]+@'; then
        echo "Credential test failed: an HTTP(S) Git remote URL contains user information or credentials:" >&2
        printf '%s\n' "$REMOTE_OUTPUT" >&2
        exit 1
    fi

    echo "Credential test passed: no credential helper or credential-bearing remote URL was found."
}

#===================================================================================
#===================================================================================
# Selects the sole USB storage disk and its sole partition as the image destination.
select_usb_device()
{
    local DEVICE_NAME
    local DEVICE_TYPE
    local DEVICE_TRANSPORT
    local -a USB_DISKS=()
    local -a USB_PARTITIONS=()

    while read -r DEVICE_NAME DEVICE_TYPE DEVICE_TRANSPORT; do
        if [ "$DEVICE_TYPE" = "disk" ] && [ "$DEVICE_TRANSPORT" = "usb" ]; then
            USB_DISKS+=("$DEVICE_NAME")
        fi
    done < <(lsblk -dnpo NAME,TYPE,TRAN)

    if [ "${#USB_DISKS[@]}" -eq 0 ]; then
        echo "No USB flash drive was found. Insert one USB storage device and run the script again." >&2
        exit 1
    fi

    if [ "${#USB_DISKS[@]}" -ne 1 ]; then
        echo "More than one USB storage disk was found; cannot safely select the release target:" >&2
        printf '  %s\n' "${USB_DISKS[@]}" >&2
        exit 1
    fi

    USB_DISK="${USB_DISKS[0]}"
    mapfile -t USB_PARTITIONS < <(lsblk -nrpo NAME,TYPE "$USB_DISK" | awk '$2 == "part" { print $1 }')

    if [ "${#USB_PARTITIONS[@]}" -ne 1 ]; then
        echo "USB disk $USB_DISK must contain exactly one partition; found ${#USB_PARTITIONS[@]}" >&2
        exit 1
    fi

    USB_DEVICE="${USB_PARTITIONS[0]}"
    echo "Automatically selected USB flash drive partition: $USB_DEVICE"
}

#===================================================================================
#===================================================================================
# Mounts the selected USB partition and verifies its filesystem and usable free space.
mount_and_validate_usb_device()
{
    local MOUNT_SOURCE
    local FILESYSTEM_TYPE
    local SD_SIZE_BYTES
    local AVAILABLE_BYTES
    local EXISTING_IMAGE_BYTES=0

    sudo mkdir -p "$USB_MOUNT"

    if mountpoint -q "$USB_MOUNT"; then
        MOUNT_SOURCE="$(readlink -f "$(findmnt -n -o SOURCE --target "$USB_MOUNT")")"
        if [ "$MOUNT_SOURCE" != "$(readlink -f "$USB_DEVICE")" ]; then
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

    FILESYSTEM_TYPE="$(findmnt -n -o FSTYPE --target "$USB_MOUNT")"
    case "$FILESYSTEM_TYPE" in
        ntfs|ntfs3|fuseblk)
            ;;
        *)
            echo "USB partition $USB_DEVICE must use NTFS; detected $FILESYSTEM_TYPE" >&2
            exit 1
            ;;
    esac

    if ! sudo test -w "$USB_MOUNT"; then
        echo "USB mount point is not writable: $USB_MOUNT" >&2
        exit 1
    fi

    SD_SIZE_BYTES="$(sudo blockdev --getsize64 "$SD_DEVICE")"
    AVAILABLE_BYTES="$(df -B1 --output=avail "$USB_MOUNT" | tail -n 1 | tr -d '[:space:]')"
    if sudo test -f "$IMAGE_PATH"; then
        EXISTING_IMAGE_BYTES="$(sudo stat -c '%s' "$IMAGE_PATH")"
    fi

    if [ $((AVAILABLE_BYTES + EXISTING_IMAGE_BYTES)) -lt "$SD_SIZE_BYTES" ]; then
        echo "USB partition does not have enough free space for the $SD_SIZE_BYTES-byte SD image" >&2
        exit 1
    fi
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

    rm -rf "$HOME/SDL2-2.32.10/build"
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

verify_no_git_credentials

cd "$GS_DIR"
cleanup_release_files

wget -O pishrink.sh "$PISHRINK_URL"
sudo chmod +x pishrink.sh
sudo mv pishrink.sh "$PISHRINK_PATH"

select_usb_device
mount_and_validate_usb_device

sudo dd if="$SD_DEVICE" of="$IMAGE_PATH" bs=1M status=progress
sudo "$PISHRINK_PATH" -z -a "$IMAGE_PATH"
sync

sudo umount "$USB_MOUNT"
MOUNTED_BY_SCRIPT=false

echo "Compressed release image created successfully: $IMAGE_PATH.gz"
