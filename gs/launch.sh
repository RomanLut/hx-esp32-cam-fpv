#!/bin/bash

# Function to check if a desktop environment is running
is_desktop_running() {
    # Check for common desktop environment processes
    if pgrep -x "gnome-shell" > /dev/null || pgrep -x "plasmashell" > /dev/null || pgrep -x "xfce4-session" > /dev/null || pgrep -x "mate-session" > /dev/null || pgrep -x "cinnamon" > /dev/null; then
        return 0
    else
        return 1
    fi
}

cd /home/pi/
cd esp32-cam-fpv
cd gs
sudo airmon-ng check kill

if is_desktop_running; then
    sudo -E LD_LIBRARY_PATH=/usr/local/lib SDL_VIDEODRIVER=kmsdrm ./gs
else
    sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gs
fi


#reconnect wlan0 to access point
sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
