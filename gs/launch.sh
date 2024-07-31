#!/bin/bash

# Function to check if X11 or any desktop environment is running
is_desktop_running() {
    if pgrep -x "Xorg" > /dev/null || pgrep -x "lxsession" > /dev/null; then
        return 0
    else
        return 1
    fi
}

cd ~

#for Raspberry
cd /home/pi/

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
