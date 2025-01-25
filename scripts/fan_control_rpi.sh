#PWM1
#pin35 on the header

#Add to boot/consfig.txt:
#dtoverlay=pwm-2chan,pin2=19,func2=2

#!/bin/bash

# Device paths
DEV_TEMP="/sys/class/thermal/thermal_zone0/temp"
DEV_PWM="/sys/class/pwm/pwmchip0/pwm1"
DEV_ENABLE="$DEV_PWM/enable"
DEV_DUTY="$DEV_PWM/duty_cycle"
DEV_PERIOD="$DEV_PWM/period"

# Export pwm1 if not already available
if [ ! -e $DEV_PWM ]; then
    echo 1 > /sys/class/pwm/pwmchip0/export
    sleep 1
fi

# Set PWM period (frequency)
PERIOD=10000000  # 10 kHz
echo $PERIOD > $DEV_PERIOD

# Temperature thresholds (in millidegrees Celsius)
TEMP_OFF=40000   # 50øC
TEMP_LOW=50000   # 60øC
TEMP_HIGH=60000  # 78øC

# Corresponding duty cycles
DUTY_OFF=0
DUTY_LOW=4000000   # 40% of PERIOD
DUTY_HIGH=7000000  # 70% of PERIOD
DUTY_MAX=10000000  # 100% of PERIOD

# Enable PWM
echo 1 > $DEV_ENABLE

# Initialize the previous duty value to detect changes
PREV_DUTY=-1

while true; do
    # Read the current CPU temperature
    TEMP=$(cat $DEV_TEMP)

    # Determine the appropriate duty cycle based on temperature
    if [ $TEMP -lt $TEMP_OFF ]; then
        DUTY=$DUTY_OFF
    elif [ $TEMP -lt $TEMP_LOW ]; then
        DUTY=$DUTY_LOW
    elif [ $TEMP -lt $TEMP_HIGH ]; then
        DUTY=$DUTY_HIGH
    else
        DUTY=$DUTY_MAX
    fi

    # If the duty cycle has changed, update and print the values
    if [ "$DUTY" -ne "$PREV_DUTY" ]; then
        echo $DUTY > $DEV_DUTY
        echo "Temperature: $(($TEMP / 1000))øC, Duty Cycle: $((DUTY * 100 / PERIOD))%"
        PREV_DUTY=$DUTY
    fi

    # Wait before checking the temperature again
    sleep 5
done


