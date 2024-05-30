#!/bin/bash

# Time to wait for wlan1 to appear
timeout=10
interval=1
elapsed=0

# Loop to check for wlan1
while [ $elapsed -lt $timeout ]; do
  if ip link show wlan1 &> /dev/null; then
    echo "wlan1 found, starting GS..."
    
    cd /home/pi/esp32-cam-fpv
    cd gs
    sudo airmon-ng check kill
    sudo ip link set wlan1 down
    sudo iw dev wlan1 set type monitor
    sudo ip link set wlan1 up

# Check if wlan2 is available
    if ip link show wlan2 &> /dev/null; then
      sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gs -fullscreen 1 -sm 1 -rx wlan1 wlan2 -tx wlan1
    else
      sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gs -fullscreen 1 -sm 1 -rx wlan1 -tx wlan1
    fi

#reconnect wlan0 to access point
    sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf

    exit 0
  fi
  echo "Waiting for wlan1..."
  sleep $interval
  elapsed=$((elapsed + interval))
done

echo "wlan1 not found within $timeout seconds, exiting..."
exit 1