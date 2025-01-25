#!/bin/bash
#set -x
#===========================================

# PWM Output is at PIN35 on the header (PWM1)

# Add to boot/config.txt:
# dtoverlay=pwm-2chan,pin2=19,func2=2

# Run:
# ./fan_control_rpi.sh

# Install as service:
# ./fan_control_rpi.sh install

# Also:
# ./fan_control_rpi.sh status
# ./fan_control_rpi.sh start
# ./fan_control_rpi.sh stop
# ./fan_control_rpi.sh uninstall

#===========================================

# Duty cycles specified in percentages
DUTY_MIN_PERCENT=20   # 20%
DUTY_MAX_PERCENT=100  # 100%

# Temperature thresholds in degrees Celsius
TEMP_MIN_C=68    # Minimum temperature (degrees Celsius) for the fan to start
TEMP_MAX_C=78    # Maximum temperature (degrees Celsius) for maximum fan speed

# PWM Frequency in Hz
PWM_FREQUENCY=30000  # 30KHz

SERVICE_NAME="fan_control_rpi"
SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME.service"
SCRIPT_PATH="/usr/local/bin/$SERVICE_NAME.sh"

#===========================================

install_service() {
    echo "Installing $SERVICE_NAME service..."

    # Stop the service if it's already running
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        echo "Stopping existing $SERVICE_NAME service..."
        sudo systemctl stop "$SERVICE_NAME"
    fi

    # Copy the script to /usr/local/bin
    sudo cp "$0" "$SCRIPT_PATH"
    sudo chmod +x "$SCRIPT_PATH"

    # Create or overwrite the systemd service file
    sudo bash -c "cat > $SERVICE_PATH" <<EOF
[Unit]
Description=Fan Control Service
After=multi-user.target

[Service]
StandardOutput=file:/var/log/fan_control_rpi.log
StandardError=file:/var/log/fan_control_rpi_error.log
Type=simple 
ExecStart=/bin/bash $SCRIPT_PATH run
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

uninstall_service() {
    echo "Uninstalling $SERVICE_NAME service..."

    # Stop the service if it's running
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        echo "Stopping $SERVICE_NAME service..."
        sudo systemctl stop "$SERVICE_NAME"
    fi

    # Disable the service
    if systemctl is-enabled --quiet "$SERVICE_NAME"; then
        echo "Disabling $SERVICE_NAME service..."
        sudo systemctl disable "$SERVICE_NAME"
    fi

    # Remove the service file
    if [ -f "$SERVICE_PATH" ]; then
        echo "Removing service file..."
        sudo rm "$SERVICE_PATH"
    fi

    # Remove the script from /usr/local/bin
    if [ -f "$SCRIPT_PATH" ]; then
        echo "Removing script..."
        sudo rm "$SCRIPT_PATH"
    fi

    # Reload systemd to apply changes
    sudo systemctl daemon-reload

    echo "$SERVICE_NAME service uninstalled successfully!"
}

check_service_status() {
    echo "Checking $SERVICE_NAME service status..."
    sudo systemctl status "$SERVICE_NAME"
}

start_service() {
    echo "Starting $SERVICE_NAME service..."
    sudo systemctl start "$SERVICE_NAME"
}

stop_service() {
    echo "Stopping $SERVICE_NAME service..."
    sudo systemctl stop "$SERVICE_NAME"
}

run_service() {
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

    # Calculate PWM period in nanoseconds based on frequency
    PERIOD=$((1000000000 / PWM_FREQUENCY))  # Period in ns
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
            PREV_DUTY=$DUTY
        fi

        echo "Temperature: $(($TEMP / 1000))°C, Duty Cycle: $((DUTY * 100 / PERIOD))%"

        # Wait before checking the temperature again
        sleep 5
    done
}

#===========================================

# Check if the script is run with the "install", "uninstall", "status", "start", "stop", or "run" parameter
if [ "$1" == "install" ]; then
    install_service
elif [ "$1" == "uninstall" ]; then
    uninstall_service
elif [ "$1" == "status" ]; then
    check_service_status
elif [ "$1" == "start" ]; then
    start_service
elif [ "$1" == "stop" ]; then
    stop_service
elif [ "$1" == "run" ]; then
    run_service "$2"
else
    echo "Usage: $0 {install|uninstall|status|start|stop|run}"
    exit 1
fi

exit 0