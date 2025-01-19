#!/bin/bash

# Variable to store detection result
IS_RADXA=false

# Path to the compatible file
COMPATIBLE_FILE="/proc/device-tree/compatible"

# Check if the compatible file exists
if [ -f "$COMPATIBLE_FILE" ]; then
    # Read the content of the file
    COMPATIBLE_CONTENT=$(cat "$COMPATIBLE_FILE")

    # Check if the content contains "radxa,zero3"
    if echo "$COMPATIBLE_CONTENT" | grep -q "radxa,zero3w"; then
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

if is_desktop_running; then
    sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gs
else
    sudo -E LD_LIBRARY_PATH=/usr/local/lib SDL_VIDEODRIVER=kmsdrm ./gs
fi


#reconnect wlan0 to access point
sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
