#!/bin/bash

cd ~
cd esp32-cam-fpv
cd gs
sudo airmon-ng check kill

#sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gs
sudo -E LD_LIBRARY_PATH=/usr/local/lib SDL_VIDEODRIVER=kmsdrm ./gs

#reconnect wlan0 to access point
sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
