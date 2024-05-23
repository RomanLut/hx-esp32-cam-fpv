# Function to check if wlan1 is up
does_wlan1_missing() {
  iw dev wlan1 info &> /dev/null;   
}

interval=1
timeout=10

counter=0

# Loop until wlan1 is up or timeout is reached
while ! does_wlan1_missing; do
    if [ $counter -ge $timeout ]; then
        echo "Timeout reached. wlan1 did not appear."
        exit 1
    fi
    
    echo "Waiting for wlan1 to be up..."
    sleep $interval
    counter=$((counter + 1))
done

cd /home/pi/esp32-cam-fpv
cd gs
sudo airmon-ng check kill
sudo ip link set wlan1 down
sudo iw dev wlan1 set type monitor
sudo ip link set wlan1 up
sudo -E LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0  ./gs -fullscreen 1 -sm 1 -rx wlan1 -tx wlan1
