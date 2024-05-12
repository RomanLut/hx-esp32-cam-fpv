if ! iw dev wlan1 info &> /dev/null; then
    echo "wlan1 interface not found, exiting script."
    exit 1
fi

cd /home/pi/esp32-cam-fpv
cd gs
sudo airmon-ng check kill
sudo ip link set wlan1 down
sudo iw dev wlan1 set type monitor
sudo ip link set wlan1 up
sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0  ./gs -fullscreen 1 -sm 1 -rx wlan1 -tx wlan1
