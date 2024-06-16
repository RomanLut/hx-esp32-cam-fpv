#!/bin/bash

# Time to wait for wlan1 to appear
timeout=10
interval=1
elapsed=0

# Loop to check for wlan1
while [ $elapsed -lt $timeout ]; do
  if ip link show wlan1 &> /dev/null && ip link show wlan2 &> /dev/null; then
    echo "wlan1 and wlan 2 found, starting GS..."
    
    cd /home/pi/esp32-cam-fpv
    cd gs
    sudo airmon-ng check kill
    sudo ip link set wlan1 down
    sudo iw dev wlan1 set type monitor
    sudo ip link set wlan1 up
    sudo ip link set wlan2 down
    sudo iw dev wlan2 set type monitor
    sudo ip link set wlan2 up
    sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0  ./gs -fullscreen 1 -sm 1 -rx wlan1 wlan2  -tx wlan1

#reconnect wlan0 to access point
    sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf

    exit 0
  fi
  echo "Waiting for wlan1 and wlan2..."
  sleep $interval
  elapsed=$((elapsed + interval))
done

echo "wlan1 or wlan2 not found within $timeout seconds, exiting..."
exit 1