#!/bin/bash

# Variable to store detection result
IS_RADXA=false

# Path to the compatible file
COMPATIBLE_FILE="/proc/device-tree/compatible"

# Check if the compatible file exists
if [ -f "$COMPATIBLE_FILE" ]; then
    # Read the content of the file
    COMPATIBLE_CONTENT=$(tr -d '\000' < "$COMPATIBLE_FILE")

    # Check if the content contains "radxa,zero3"
    if echo "$COMPATIBLE_CONTENT" | grep -q "radxa,zero3"; then
        IS_RADXA=true
    fi
fi

# Assign values to QABUTTON1, QABUTTON2, and HOME_DIRECTORY based on IS_RADXA
if $IS_RADXA; then
    HOME_DIRECTORY="/home/radxa/"
else
    HOME_DIRECTORY="/home/pi/"
fi

# Output the results
echo "IS_RADXA=$IS_RADXA"

GETTY_TTY1_WAS_ACTIVE=false

stop_console_getty_while_gs_runs() {
    if ! $IS_RADXA || ! command -v systemctl >/dev/null 2>&1; then
        return
    fi

    # Radxa images autologin root on tty1. GS can be launched from SSH while
    # that physical console shell is still active; GPIO/uinput and keyboard
    # navigation keys then reach both GS and the shell, so Up/Down/Enter can
    # execute shell-history commands such as "sudo reboot". When GS is started
    # by /root/.profile on tty1, stopping getty@tty1 would kill the launch
    # shell itself, so only stop tty1 for SSH/other launch contexts.
    if [ "$(tty 2>/dev/null)" = "/dev/tty1" ]; then
        return
    fi

    if systemctl is-active --quiet getty@tty1.service; then
        GETTY_TTY1_WAS_ACTIVE=true
        sudo systemctl stop getty@tty1.service 2>/dev/null || true
    fi
}

restore_console_getty_after_gs() {
    if $GETTY_TTY1_WAS_ACTIVE; then
        # Starting tty1 getty again triggers a fresh autologin; /root/.profile then runs
        # boot_selection.sh, which would immediately relaunch GS. One-shot skip flag on
        # tmpfs (cleared on reboot) lets that login drop to a shell instead.
        sudo touch /run/esp32camfpv-skip-fpv-autostart-once 2>/dev/null || true
        sudo systemctl start getty@tty1.service 2>/dev/null || true
    fi
}

# Function to check if X11 or any desktop environment is running
is_desktop_running() {
    if pgrep -x "Xorg" > /dev/null || pgrep -x "lxsession" > /dev/null; then
        return 0
    else
        return 1
    fi
}

cd ~
cd ${HOME_DIRECTORY}

cd esp32-cam-fpv
cd gs
sudo airmon-ng check kill
stop_console_getty_while_gs_runs
trap restore_console_getty_after_gs EXIT

run_gs() {
    local video_driver_env=()
    if ! is_desktop_running; then
        video_driver_env=(SDL_VIDEODRIVER=kmsdrm)
    fi

    sudo -E env LD_LIBRARY_PATH=/usr/local/lib "${video_driver_env[@]}" ./gs
}

if is_desktop_running; then
    DISPLAY=:0 run_gs
else
    run_gs
fi

#let LAN card get ip address (required if dhcpcd service is disabled)
sudo systemctl start dhcpcd &

#reconnect wlan0 to access point
sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
