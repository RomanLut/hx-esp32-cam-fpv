# Dual Wifi Adapters (recommended)

**hx-esp32-cam-fpv** supports dual Wifi adapters to decrease packet loss ratio. [Default launch script](https://github.com/RomanLut/hx-esp32-cam-fpv/blob/1e14550fcf04f8fcb10a3b6a18126332e7aa6609/gs/launch.sh#L20) will launch GS in dual adapters mode if **wlan1** and **wlan2** are found in the system.

## Range 

**2dbi dipole on plane, 5dbi dipoles on GS:** 650m reliable, 1.2km max at 24Mbps, 600m max at 36Mbps (line of sight, away from wifi routers). Range is decreased significantly with walls/trees on the way.

**2dbi dipole on plane, 5dbi dipole + BetaFPV Moxon Antenna on GS:** 2km max at 24Mbps, 900m max at 36Mbps.

Tested on inav microplane: https://www.youtube.com/watch?v=GYB-UckucRA

![alt text](/doc/images/dfminispirit.jpg "df mini spirit")

**esp32-c5, 5.8 GHz lolipop antennas on air and ground:** 350m reliable, 850m max at 26Mbps.

Tested on inav microplane: 

![alt text](/doc/images/dart68.jpg "dart68")

Range is limited by **ESP32** output power (100mW 20dB) and highly depends on antena type and quality.
