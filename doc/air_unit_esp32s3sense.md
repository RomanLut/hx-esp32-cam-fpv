# Seeed Studio XIAO ESP32S3 Sense Air Unit

Flashing **Seeed Studio XIAO ESP32S3 Sense** firmware: [/doc/flashing_esp32s3sense.md](/doc/flashing_esp32s3sense.md)

The unified **air_firmware_esp32s3sense** firmware supports both **OV2640** and **OV5640** camera modules. It detects the installed camera at runtime, so a separate OV5640 firmware target is not required.

## OV2640 + M8 120° lens

STL files for 3D Printing on Thingiverse: https://www.thingiverse.com/thing:6624598

![alt text](/doc/images/esp32s3sense_pinout.png "esp32s3sense_pinout.png")

![alt text](/doc/images/esp32s3sense_shell.jpg "esp32s3sense_shell") ![alt text](/doc/images/esp32s3sense_shell1.jpg "esp32s3sense_shell1")

![alt text](/doc/images/esp32s3sense_shell2.jpg "esp32s3sense_shell2") ![alt text](/doc/images/esp32s3sense_shell3.jpg "esp32s3sense_shell3")

![alt text](/doc/images/esp32s3sense_shell_plane.jpg "esp32s3sense_plane") ![alt text](/doc/images/esp32s3sense_j3.jpg "esp32s3sense_j3")

Module comes with a moderate flexible antenna which should be replaced with a 2dBi dipole to maximize range.

Internal yellow LED conflicts with SD card and thus can not be used for indication. External LED should be soldered to pin **D0** via 150 ... 680 Ohm resistor.

A jumper should be soldered on **J3** to enable SD card usage. It may appear to work without it, but the jumper is required for stable operation.

Existing **Boot** button is used to start/stop air unit recording and enter OTA mode.

## OV5640 + M12 120° lens

![alt text](/doc/images/shell_14.jpg "shell_14") ![alt text](/doc/images/ov5640.jpg "ov5640")

**ESP32S3 Sense** boards are sold with **OV2640** camera which can be easily replaced with **OV5640** purchased separately. No hardware modifications are required for camera replacement.

**OV5640** camera on **ESP32S3 Sense** offers 640x360 30/50fps, 640x480 30/40fps, 800x456 30/50fps, 1024x576 30fps and 1280x720 30fps modes, less noisy sensor, much better colors and contrast, better performance against sunlight.

Connection diagram is similar to the OV2640 variant.

800x456 30fps MCS3 26Mbps (actual transfer rate ~10Mbps total with FEC 6/12), **ESP32S3 Sense + OV5640** camera 160° M8 lens:

https://github.com/RomanLut/hx-esp32-cam-fpv/assets/11955117/3abe7b94-f14d-45f1-8d33-997f12b7d9aa
