# Installing Fan Control Service 

Boot, ssh, download script:

* ```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/refs/heads/master/scripts/fan_control.sh```

* ```chmod +x fan_control.sh```

## Configure PWM channel:

  **Raspberry Pi**: Add ```dtoverlay=pwm-2chan,pin2=19,func2=2``` to  the end of the file ```boot/config.txt```   
  
  **Radxa Zero 3W**: Enable **PWM14-M0** overlay in **rsetup**: **Overlays\Manager Overlays\Enable PWM14-M0**

## Adjust parameters
Edit ```fan_control.sh```, adjust:
 - PWM frequency ```PWM_FREQUENCY=```
 - minimum PWM duty ratio ```DUTY_MIN_PERCENT=``` - the minimum duty ratio at which fan still operates.
 - maximum PWM duty ratio ```DUTY_MAX_PERCENT``` - limit maximum fan speed. F.e when connecting 5V fan to 2S VBAT, set to 60%.

## Test:
  ```./fan_control.sh run```

## Install service:

```./fan_control.sh install```

See also: [Connecting Fan](/doc/connecting_fan.md) 

