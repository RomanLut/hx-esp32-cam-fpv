#!/bin/bash
#set -x

#===========================================

# Raspberry Pi: PWM Output is on PIN35 on the header (PWM1)
# Radxa Zero 3W: PWM Output is on PIN7 on the header (PWM14-M0)

# Run to test:
# ./fan_control.sh
# Note: configure PWM beforehand!
# Raspberry Pi: add to boot/config.txt: dtoverlay=pwm-2chan,pin2=19,func2=2
# Radxa Zero 3W: enable PWM14-M0 overlay in rsetup 


# Install as service:
# ./fan_control.sh install
# Also adds PWM configuration to boot/config.txt if missing on Raspberry Pi. On Radxa Zero 3W, you have to manually enable PWM14-M0 overlay.

# Also:
# ./fan_control.sh status
# ./fan_control.sh start
# ./fan_control.sh stop
# ./fan_control.sh uninstall

#===========================================

# Duty cycles specified in percentages
DUTY_MIN_PERCENT=20   # 20%  minimum speed at which fan can reliably start to spin
DUTY_MAX_PERCENT=100  # 100%  maximum allowed speed

# Temperature thresholds in degrees Celsius
# Script will try to achieve temperature in the middle
TEMP_MIN_C=70    # Minimum temperature (degrees Celsius) for the fan to start
TEMP_MAX_C=80    # Maximum temperature (degrees Celsius) for maximum fan speed

# PWM Frequency in Hz
PWM_FREQUENCY=25  # 25Hz

SERVICE_NAME="fan_control"
SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME.service"
SCRIPT_PATH="/usr/local/bin/$SERVICE_NAME.sh"

#===========================================

configure_pwm() {
    if [ "$IS_RADXA" = true ]; then
        pwmchip_path="/sys/class/pwm/pwmchip14"
        if [ ! -d $pwmchip_path ]; then
	        echo "Please enable PWM14-M0 in rsetup!"
	        return 1
        fi    
    else
        local config_file="/boot/config.txt"
        local pwm_line="dtoverlay=pwm-2chan,pin2=19,func2=2"
        local tmp_file="/tmp/config.tmp"
        
        # Check if config.txt exists
        if ! sudo test -f "$config_file"; then
            echo "Error: $config_file not found"
            return 1
        fi

        # Check if the line already exists
        if sudo grep -q "^$pwm_line" "$config_file"; then
            echo "Ok, PWM configuration already exists in $config_file"
            return 0
        fi

        # Create backup
        sudo cp "$config_file" "${config_file}.backup"
        if [ $? -ne 0 ]; then
            echo "Error: Failed to create backup"
            return 1
        fi
        echo "Backup created at ${config_file}.backup"

        # Find [all] section or create it if it doesn't exist
        if ! sudo grep -q "^\[all\]" "$config_file"; then
            echo -e "\n[all]" | sudo tee -a "$config_file" > /dev/null
        fi

        # Add the PWM configuration under [all] section
        sudo awk -v pwm="$pwm_line" '
            /^\[all\]/ {
                print $0
                print pwm
                next
            }
            { print }
        ' "$config_file" > "$tmp_file"

        # Replace original file with new content
        if sudo cp "$tmp_file" "$config_file"; then
            rm "$tmp_file"
            echo "PWM configuration added successfully to $config_file"
            echo "Please reboot your Raspberry Pi for changes to take effect"
            return 0
        else
            rm "$tmp_file"
            echo "Error: Failed to update configuration"
            return 1
        fi
    fi
}

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
StandardOutput=journal
StandardError=journal
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

	configure_pwm
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
    DEV_TEMP="/sys/class/thermal/thermal_zone0/temp"

if [ "$IS_RADXA" = true ]; then
    DEV_PWM="/sys/class/pwm/pwmchip14/pwm0"

    # Export pwm0 if not already available
    if [ ! -e $DEV_PWM ]; then
        sudo sh -c "echo 0 > /sys/class/pwm/pwmchip14/export"
        sleep 1
    fi

else
    DEV_PWM="/sys/class/pwm/pwmchip0/pwm1"

    # Export pwm1 if not already available
    if [ ! -e $DEV_PWM ]; then
        sudo sh -c "echo 1 > /sys/class/pwm/pwmchip0/export"
        sleep 1
    fi
fi

    # 
    if [ ! -e $DEV_PWM ]; then
        echo "ERROR: PWM channel $DEV_PWM is not available. Please setup PWM channel."
        exit 1
    fi

    DEV_ENABLE="$DEV_PWM/enable"
    DEV_DUTY="$DEV_PWM/duty_cycle"
    DEV_PERIOD="$DEV_PWM/period"
    DEV_EXPORT="$DEV_PWM/export"
    DEV_POLARITY="$DEV_PWM/polarity"

    # Margin for no adjustment in degrees Celsius
    TEMP_MARGIN=1

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
    sudo sh -c "echo 'normal' > $DEV_POLARITY"

    # Initialize the previous duty value to detect changes
    PREV_DUTY=-1

    # Variable to control the loop
    RUNNING=true

    # Trap termination signals
    trap "echo 'Stopping service...'; RUNNING=false" SIGTERM SIGINT

    while $RUNNING; do
        # Read the current CPU temperature in millidegrees
        TEMP=$(cat "$DEV_TEMP")

        # Ensure TEMP is not empty
        if [ -z "$TEMP" ]; then
            echo "Error: Unable to read temperature."
            sleep 5
            continue
        fi

        # Determine the appropriate duty cycle based on temperature
        if [ $TEMP -lt $TEMP_MIN ]; then
            DUTY_CURRENT=$DUTY_OFF
        elif [ $TEMP -ge $TEMP_MAX ]; then
            DUTY_CURRENT=$DUTY_MAX
        else
            DUTY_CURRENT=$((DUTY_MIN + (TEMP - TEMP_MIN) * (DUTY_MAX - DUTY_MIN) / (TEMP_MAX - TEMP_MIN)))
        fi

        # Ensure duty cycle is within valid range
        if [ $DUTY_CURRENT -lt 0 ]; then
            DUTY_CURRENT=0
        elif [ $DUTY_CURRENT -gt $PERIOD ]; then
            DUTY_CURRENT=$PERIOD
        fi

        # If the duty cycle has changed, update and print the values
        if [ "$DUTY_CURRENT" -ne "$PREV_DUTY" ]; then
            sudo sh -c "echo $DUTY_CURRENT > $DEV_DUTY"
            PREV_DUTY=$DUTY_CURRENT
        fi

        echo "Temperature: $(($TEMP / 1000))°C, Duty Cycle: $((DUTY_CURRENT * 100 / PERIOD))%"

        # Wait for a short interval and check the termination flag
        for ((i = 0; i < 10; i++)); do
            if ! $RUNNING; then
                break
            fi
            sleep 1  # Short interval to allow responsive signal handling
        done

    done

    # Disable PWM when exiting
    echo "Disabling PWM..."
    sudo sh -c "echo 0 > $DEV_ENABLE"
    echo "Service stopped."
}

#===========================================

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

echo "IS_RADXA=$IS_RADXA"

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