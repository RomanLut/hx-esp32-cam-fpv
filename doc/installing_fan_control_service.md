# Installing Fan Control Service

Boot the target, connect over SSH, and download the script:

* ```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/refs/heads/release/scripts/fan_control.sh```

* ```chmod +x fan_control.sh```

## Configure the PWM channel

**Raspberry Pi:** `./fan_control.sh install` automatically adds
`dtoverlay=pwm-2chan,pin2=19,func2=2` to `/boot/config.txt` if it is missing and
creates `/boot/config.txt.backup` before changing the file. Reboot after
installation so the PWM controller becomes available. Do not add duplicate
overlay lines manually.

**Radxa Zero 3W:** Enable **PWM14-M0**, **PWM15-M1**, or both overlays in
`rsetup` before installing the service. At least one must be enabled. When both
are available, the service drives both outputs with the same frequency and
duty cycle.

## Adjust parameters
Edit `fan_control.sh` before installation and adjust:

- `PWM_FREQUENCY`: PWM frequency.
- `DUTY_MIN_PERCENT`: minimum duty at which the fan starts reliably.
- `DUTY_MAX_PERCENT`: maximum fan speed. For example, limit this when powering
  a 5 V fan from a 2S battery.

## Install the service

```./fan_control.sh install```

After installation, script path is ```/usr/local/bin/fan_control.sh```.

On Raspberry Pi, reboot before checking the service:

```bash
sudo reboot
```

Verify the service and PWM output after reboot:

```bash
systemctl is-enabled fan_control
systemctl is-active fan_control
cat /sys/class/pwm/pwmchip0/pwm1/enable
cat /sys/class/pwm/pwmchip0/pwm1/period
cat /sys/class/pwm/pwmchip0/pwm1/duty_cycle
```

The service can be active with duty cycle `0` when the CPU temperature is below
`TEMP_MIN_C`; that is normal.

To restart service after adjustments, use: ```sudo systemctl restart fan_control```.

See also: [Connecting Fan](/doc/connecting_fan.md) 

