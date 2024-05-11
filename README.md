# **WORK IN PROGRESS!!!!**

# esp32-cam-fpv
Digital,low latency FPV System based on ESP32.

## Features:
- 640x480(4:3), 640x360(16:9), 800x600(4:3), 800x456(16:9) 30FPS 
- 1280x1024(4:3), 1280:720(16:9) 13fps on ov2640, 30fps on ov5640 with esp32s3
- up to 1km at 24mBit (line of sight)
- latency 20-50ms
- bidirectional Mavlink stream for RC and telemetry ~11kb/sec
- Displayport MSP OSD
- on-board and grounstation video recording

**Air units variants:**
- esp32cam board with ov2640 camera
- esp32s3sense with ov2640 camera
- esp32s3sense with ov5640 camera

**Ground station:**
- Raspberry Pi Zero 2W ... Raspberry Pi 4B with rtl8812au or AR9270 wifi card


## Original project

**esp32-cam-fpv** project was originally developed by **jeanlemotan** https://github.com/jeanlemotan/esp32-cam-fpv (currently seems to be abandoned). Some more work has been done by **Ncerzzk** https://github.com/Ncerzzk/esp-vtx who also seems to developed custom air unit hardware https://github.com/Ncerzzk/esp-vtx-hardware and continues to work on gs https://github.com/Ncerzzk/esp-vtx-gs-rs.

The goal of this fork is to develop fpv system for small inav-based plane.

# Theory
ESP32 is too slow for video encoding. The data is received from the camera module as JPEG at 10MHz I2S clock (ESP32) or 20MHz (esp32s3) and passed directly to the wifi module and written to the SD card if the DVR is enabled.

The **ESP camera** component has been modified to send the data as it's received from the DMA instead of frame-by-frame basis. This decreases latency quite significantly (10-20 ms) and reduces the need to allocate full frames in PSRAM. **Ncerzzk** even removed PSRAM on his board. While frame is received from the camera, it is already in flight to GS.

The wifi data is send using packet injection which is possible on ESP32 platform. Data is sent with error correction encoding (FEC) wich allows GS to recover lost packets. No acknowlegements are sent from GS and no retransmissiona are done by air unit.


The air unit can also record the video straight from the camera to a sd card. The format is a rudimentary MJPEG without any header so when playing back the FPS will be whatever your player will decide.

There is significant buffering when writing to SD (3MB at the moment) to work around the very regular slowdowns of sd cards.

The receiver is a Raspberry PI Zero 2W ... Pi4  with Realtek 8812au or AR9270 adapter in monitor mode. Two wifi adapters may work as diversity receivers if required.

The JPEG decoding is done with turbojpeg to lower latency and - based on the resolution - can take between 1 and 7 milliseconds.

It's then uploaded to texture and shown on screen.

The link is bi-directional so the ground station can send data to the air unit. At the moment it sends camera and wifi configuration and bi-directional serial port for telemetry (FEC encoded).

Here is a video shot at 30 FPS at 800x600 (video converted from the source mjpeg):

https://user-images.githubusercontent.com/10252034/116135308-43c08c00-a6d1-11eb-99b9-9dbb106b6489.mp4

# Building

## Air Unit

**TODO**

## Ground Station

**TODO**



