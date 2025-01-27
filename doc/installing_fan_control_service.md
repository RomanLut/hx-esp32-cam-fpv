# Installing fan control sservice 

* Edit ```fan_control_rpi.sh``` : adjust PWM frequency ```PWM_FREQUENCY=`` and minimum PWM duty ratio ```DUTY_MIN_PERCENT=``` if required.

* Install service:

  ```fan_control_rpi.sh install```

* Add to ```boot/config.txt``` at the end:

  ```dtoverlay=pwm-2chan,pin2=19,func2=2```


# 2-wire fan

 5v fan

 Simple schematics =
 May be noisy. To  lower noice, change PWM frequency to 30000 (30Khz). May not work.

 Low-noice schematics =
 change PWM frequency to 30000 (30Khz)


# 4-wire fan

 Should be compatible with 3.3 PWM signal.

 How to find out if ti is compatible:
 Connect fan to 5v  voltage on PWM pin.
 Should be 3.3V or less.
 Then connect PWM pin,check it if works.

 PWM frequency in script should be set to  25Hz (default).