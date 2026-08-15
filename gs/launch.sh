#!/bin/bash

# ESP32-CAM-FPV / RubyFPV launch contract. Keep all four invariants intact:
# 1. boot_selection.sh reads the boot GPIO buttons and chooses RubyFPV or this GS.
# 2. While this GS runs, tty1 getty must be stopped so buttons cannot execute shell commands.
# 3. A GS crash or other nonzero exit must restart GS without exposing the console.
# 4. Exit To Shell returns zero, must not restart GS, and must restore a usable tty1 console.

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
GS_EXIT_STATUS=1
GS_RESTART_MANAGED="${ESP32CAMFPV_RESTART_ON_FAILURE:-0}"

#===================================================================================
#===================================================================================
# Prevents a separate tty1 login shell from receiving GS menu key presses.
stop_console_getty_while_gs_runs()
{
    if ! command -v systemctl >/dev/null 2>&1; then
        return
    fi

    # The boot selector starts GS in a restart-managed transient unit. Preserve
    # console ownership across service restarts even after getty is already stopped.
    if [ "$GS_RESTART_MANAGED" = "1" ]; then
        GETTY_TTY1_WAS_ACTIVE=true
        if systemctl is-active --quiet getty@tty1.service; then
            sudo systemctl stop getty@tty1.service 2>/dev/null || true
        fi
        return
    fi

    # RubyFPV images autologin root on tty1. GS can be launched from SSH while
    # that physical console shell is still active; GPIO/uinput and keyboard
    # navigation keys then reach both GS and the shell, so Up/Down/Enter can
    # execute shell-history commands such as "sudo reboot". When GS is started
    # directly by /root/.profile on tty1, stopping getty@tty1 would kill the
    # launch shell itself, so only stop tty1 for detached/SSH launch contexts.
    if [ "$(tty 2>/dev/null)" = "/dev/tty1" ]; then
        return
    fi

    if systemctl is-active --quiet getty@tty1.service; then
        GETTY_TTY1_WAS_ACTIVE=true
        sudo systemctl stop getty@tty1.service 2>/dev/null || true
    fi
}

#===================================================================================
#===================================================================================
# Restarts tty1 after GS exits so agetty restores text and keyboard console modes.
restore_console_getty_after_gs()
{
    if $GETTY_TTY1_WAS_ACTIVE; then
        if [ "$GS_EXIT_STATUS" -eq 0 ]; then
            # Exit To Shell is the only path that arms the skip flag. The replacement
            # tty1 login consumes it and stays at a usable console prompt.
            sudo touch /run/esp32camfpv-skip-fpv-autostart-once 2>/dev/null || true
            sudo systemctl start getty@tty1.service 2>/dev/null || true
        elif [ "$GS_RESTART_MANAGED" != "1" ]; then
            # A manually launched GS has no service supervisor. Restore tty1 without
            # the skip flag so boot_selection.sh starts a managed replacement GS.
            sudo systemctl start getty@tty1.service 2>/dev/null || true
        fi
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
    GS_EXIT_STATUS=$?
else
    run_gs
    GS_EXIT_STATUS=$?
fi

# RubyFPV disables dhcpcd, so request a lease after GS exits only when the wired
# interface has no address. Starting dhcpcd over a persistent static management
# address adds a second DHCP address and changes the default source route.
if ! ip -4 addr show dev eth0 2>/dev/null | grep -q 'inet '; then
    sudo systemctl start dhcpcd &
fi

#reconnect wlan0 to access point
sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf

# Preserve the GS result so the transient unit restarts crashes but considers
# Exit To Shell successful after the console has been restored by the EXIT trap.
exit "$GS_EXIT_STATUS"
