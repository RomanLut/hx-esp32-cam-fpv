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
if [ "$IS_RADXA" = true ]; then
    QABUTTON1=114
    QABUTTON2=102
    HOME_DIRECTORY="/home/radxa"

    # Automatically run resize2fs once if not already done
    if [ ! -f /etc/resize2fs-done ]; then
        echo "Resizing root filesystem..."
        ROOT_DEV=$(findmnt -n -o SOURCE /)
        resize2fs "$ROOT_DEV" && touch /etc/resize2fs-done
    fi

else
    QABUTTON1=17
    QABUTTON2=4
    HOME_DIRECTORY="/home/pi"
    sudo raspi-gpio set 17 ip pd
    sudo raspi-gpio set 4 ip pd
fi

# Output the results
echo "IS_RADXA=$IS_RADXA"
echo "QABUTTON1=$QABUTTON1"
echo "QABUTTON2=$QABUTTON2"


#Launch Ruby on first boot to install drivers
# Define the path to the file
FILE="$HOME_DIRECTORY/ruby/config/boot_count.cfg"

# Check if the file does not exist
if [ ! -f "$FILE" ]; then
    echo "First boot"
    echo "Launching Ruby..."
    cd ${HOME_DIRECTORY}/ruby
    ./ruby_start
    exit
fi


# Export GPIOs as input
sudo sh -c "echo $QABUTTON1 > /sys/class/gpio/export"
sudo sh -c "echo in > /sys/class/gpio/gpio$QABUTTON1/direction"
sudo sh -c "echo $QABUTTON2 > /sys/class/gpio/export"
sudo sh -c "echo in > /sys/class/gpio/gpio$QABUTTON2/direction"

# Check GPIO values and write to bootSelection.txt
if [ $(sudo cat /sys/class/gpio/gpio$QABUTTON1/value) -eq 1 ]; then
    echo "esp32camfpv" | sudo tee bootSelection.txt > /dev/null
fi

if [ $(sudo cat /sys/class/gpio/gpio$QABUTTON2/value) -eq 1 ]; then
    echo "ruby" | sudo tee bootSelection.txt > /dev/null
fi

# Restore GPIOs 
sudo sh -c "echo $QABUTTON1 > /sys/class/gpio/unexport"
sudo sh -c "echo $QABUTTON2 > /sys/class/gpio/unexport"

# Check bootSelection.txt and execute appropriate script
if [ -f "bootSelection.txt" ] && grep -q "ruby" bootSelection.txt; then
    echo "Launching Ruby..."
    cd ${HOME_DIRECTORY}/ruby
    ./ruby_start
else
    echo "Launching esp32-cam-fpv..."
    cd ${HOME_DIRECTORY}/esp32-cam-fpv/gs
    ./launch.sh
fi

