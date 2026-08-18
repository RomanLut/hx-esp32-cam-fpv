#!/bin/bash

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
if [ "$IS_RADXA" = true ]; then
    QABUTTON1=114
    QABUTTON2=102
    QABUTTON3=97
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
    QABUTTON3=23
    HOME_DIRECTORY="/home/pi"
    sudo raspi-gpio set 17 ip pd
    sudo raspi-gpio set 4 ip pd
    sudo raspi-gpio set 23 ip pd
fi

BOOT_SELECTION_FILE="$HOME_DIRECTORY/bootSelection.txt"
GS_INI_FILE="$HOME_DIRECTORY/esp32-cam-fpv/gs/gs.ini"
GPIO_KEYS_LAYOUT=$(sed -n 's/^[[:space:]]*gpio_keys_layout[[:space:]]*=[[:space:]]*\([0-9][0-9]*\)[[:space:]]*$/\1/p' "$GS_INI_FILE" 2>/dev/null | head -n 1)
GPIO_KEYS_LAYOUT=${GPIO_KEYS_LAYOUT:-0}

# Output the results
echo "IS_RADXA=$IS_RADXA"
echo "==================================="
echo "QABUTTON1(AIR REC,R)=$QABUTTON1"
echo "QABUTTON2(GS REC,G)=$QABUTTON2"
echo "QABUTTON3(Center)=$QABUTTON3"


# Launch Ruby on first boot to install drivers
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
sudo sh -c "echo $QABUTTON3 > /sys/class/gpio/export"
sudo sh -c "echo in > /sys/class/gpio/gpio$QABUTTON3/direction"

# Allow the GPIO input configuration and button levels to settle before the
# one-time boot selection sample.
sleep 0.2

# Sample each button once so the reported state is the state used for boot selection.
QABUTTON1_STATE=$(sudo cat /sys/class/gpio/gpio$QABUTTON1/value)
QABUTTON2_STATE=$(sudo cat /sys/class/gpio/gpio$QABUTTON2/value)
QABUTTON3_STATE=$(sudo cat /sys/class/gpio/gpio$QABUTTON3/value)

# DIY VRX 2 keeps the DIY wiring but deliberately ignores the GS REC input,
# including during boot selection before the GS GPIO handler is running.
if [ "$GPIO_KEYS_LAYOUT" -eq 2 ]; then
    QABUTTON2_STATE=0
fi

echo "==================================="
echo "QABUTTON1(AIR REC,R) state=$QABUTTON1_STATE"
echo "QABUTTON2(GS REC,G) state=$QABUTTON2_STATE"
echo "QABUTTON3(Center) state=$QABUTTON3_STATE"
echo "GPIO keys layout=$GPIO_KEYS_LAYOUT"
echo "==================================="

# REC/R has priority so pressing REC/R and REC/G together always selects
# esp32-cam-fpv instead of allowing the later Ruby selection to overwrite it.
if [ "$QABUTTON1_STATE" -eq 1 ]; then
    echo "esp32camfpv" | sudo tee "$BOOT_SELECTION_FILE" > /dev/null
elif [ "$QABUTTON2_STATE" -eq 1 ] || [ "$QABUTTON3_STATE" -eq 1 ]; then
    echo "ruby" | sudo tee "$BOOT_SELECTION_FILE" > /dev/null
fi
# With no pressed button, leave bootSelection.txt unchanged and reuse the
# previous boot choice.

# Restore GPIOs
sudo sh -c "echo $QABUTTON1 > /sys/class/gpio/unexport"
sudo sh -c "echo $QABUTTON2 > /sys/class/gpio/unexport"
sudo sh -c "echo $QABUTTON3 > /sys/class/gpio/unexport"

# Check bootSelection.txt and execute appropriate script
if [ -f "$BOOT_SELECTION_FILE" ] && grep -q "ruby" "$BOOT_SELECTION_FILE"; then
    echo "Launching Ruby..."
    cd ${HOME_DIRECTORY}/ruby
    ./ruby_start
else
    # launch.sh only creates this flag after a GS instance started outside tty1
    # restores the console. Never apply it to the RubyFPV boot-selection path.
    GS_SKIP_NEXT_FPV_AUTOSTART_FLAG=/run/esp32camfpv-skip-fpv-autostart-once
    if [ -f "$GS_SKIP_NEXT_FPV_AUTOSTART_FLAG" ]; then
        echo "Skipping esp32-cam-fpv autostart once (after GS released the console)."
        rm -f "$GS_SKIP_NEXT_FPV_AUTOSTART_FLAG"
        exit 0
    fi
    echo "Launching esp32-cam-fpv..."
    GS_DIRECTORY="${HOME_DIRECTORY}/esp32-cam-fpv/gs"
    if [ "$(tty 2>/dev/null)" = "/dev/tty1" ] && command -v systemd-run >/dev/null 2>&1; then
        # KMS takes ownership of tty1. Run the selected GS outside getty's cgroup so
        # launch.sh can stop getty without killing itself or starting duplicate GS copies.
        : > /tmp/esp32camfpv-gs.log
        systemd-run --quiet --collect --unit=esp32camfpv-gs-session \
            --property=Restart=on-failure --property=RestartSec=2s \
            /usr/bin/env ESP32CAMFPV_RESTART_ON_FAILURE=1 \
            /bin/bash -c "cd '$GS_DIRECTORY' && exec ./launch.sh >>/tmp/esp32camfpv-gs.log 2>&1"
    else
        cd "$GS_DIRECTORY"
        ./launch.sh
    fi
fi
