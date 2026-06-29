# Dual Wifi Adapters (recommended)

**hx-esp32-cam-fpv** supports dual Wifi adapters to decrease packet loss ratio. Also supported on Android and Oculus quest. Using a dual-adapter configuration significantly decreases dropped frames.

[Default launch script](https://github.com/RomanLut/hx-esp32-cam-fpv/blob/1e14550fcf04f8fcb10a3b6a18126332e7aa6609/gs/launch.sh#L20) will launch GS in dual adapters mode if **wlan1** and **wlan2** are found in the system.
