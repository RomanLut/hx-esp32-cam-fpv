#PWM1
#pin35 on the header

#Add to boot/consfig.txt:
#dtoverlay=pwm-2chan,pin2=19,func2=2

#install as service:
#./fan_control_rpi.sh install

#check serice status:
#sudo systemctl status fan_control_rpi

#!/bin/bash

SERVICE_NAME="fan_control_rpi"
SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME.service"
SCRIPT_PATH="/usr/local/bin/$SERVICE_NAME.sh"

install_service() {
    echo "Installing $SERVICE_NAME service..."

    # Copy the script to /usr/local/bin
    sudo cp "$0" "$SCRIPT_PATH"
    sudo chmod +x "$SCRIPT_PATH"

    # Create systemd service file
    sudo bash -c "cat > $SERVICE_PATH" <<EOF
[Unit]
Description=Fan Control Service
After=multi-user.target

[Service]
Type=simple
ExecStart=/bin/bash $SCRIPT_PATH
Restart=always
User=root

[Install]
WantedBy=multi-user.target
EOF

    # Reload systemd, enable, and start the service
    sudo systemctl daemon-reload
    sudo systemctl enable "$SERVICE_NAME"
    sudo systemctl start "$SERVICE_NAME"

    echo "$SERVICE_NAME service installed and started successfully!"
}

# Check if the script is run with the "install" parameter
if [ "$1" == "install" ]; then
    install_service
    exit 0
fi

# Duty cycles specified in percentages
DUTY_MIN_PERCENT=40   # 40%
DUTY_MAX_PERCENT=100  # 100%

# Temperature thresholds in degrees Celsius
TEMP_MIN_C=68    # Minimum temperature (degrees Celsius) for the fan to start
TEMP_MAX_C=78    # Maximum temperature (degrees Celsius) for maximum fan speed

# Device paths
DEV_TEMP="/sys/class/thermal/thermal_zone0/temp"
DEV_PWM="/sys/class/pwm/pwmchip0/pwm1"
DEV_ENABLE="$DEV_PWM/enable"
DEV_DUTY="$DEV_PWM/duty_cycle"
DEV_PERIOD="$DEV_PWM/period"

# Export pwm1 if not already available
if [ ! -e $DEV_PWM ]; then
    sudo sh -c "echo 1 > /sys/class/pwm/pwmchip0/export"
    sleep 1
fi

# Set PWM period (frequency)
PERIOD=33333  # 30 kHz = 33,333 ns
sudo sh -c "echo $PERIOD > $DEV_PERIOD"

# Convert temperature thresholds to millidegrees
TEMP_MIN=$((TEMP_MIN_C * 1000))
TEMP_MAX=$((TEMP_MAX_C * 1000))

# Convert duty cycle percentages to actual values
DUTY_MIN=$((PERIOD * DUTY_MIN_PERCENT / 100))
DUTY_MAX=$((PERIOD * DUTY_MAX_PERCENT / 100))
DUTY_OFF=0  # Fan off

# Enable PWM
sudo sh -c "echo 1 > $DEV_ENABLE"

# Initialize the previous duty value to detect changes
PREV_DUTY=-1

while true; do
    # Read the current CPU temperature in millidegrees
    TEMP=$(cat $DEV_TEMP)

    # Ensure TEMP is not empty
    if [ -z "$TEMP" ]; then
        echo "Error: Unable to read temperature."
        sleep 5
        continue
    fi

    # Determine the appropriate duty cycle based on temperature
    if [ $TEMP -lt $TEMP_MIN ]; then
        DUTY=$DUTY_OFF
    elif [ $TEMP -ge $TEMP_MAX ]; then
        DUTY=$DUTY_MAX
    else
        DUTY=$((DUTY_MIN + (TEMP - TEMP_MIN) * (DUTY_MAX - DUTY_MIN) / (TEMP_MAX - TEMP_MIN)))
    fi

    # Ensure duty cycle is within valid range
    if [ $DUTY -lt 0 ]; then
        DUTY=0
    elif [ $DUTY -gt $PERIOD ]; then
        DUTY=$PERIOD
    fi

    # If the duty cycle has changed, update and print the values
    if [ "$DUTY" -ne "$PREV_DUTY" ]; then
        sudo sh -c "echo $DUTY > $DEV_DUTY"
        echo "Temperature: $(($TEMP / 1000))°C, Duty Cycle: $((DUTY * 100 / PERIOD))%"
        PREV_DUTY=$DUTY
    fi

    # Wait before checking the temperature again
    sleep 5
done
