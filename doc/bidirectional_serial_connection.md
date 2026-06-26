# Bidirectional serial connection

There is bidirectional stream sent with FEC encoding (Ground2Air: ```k=2 n=3```, Air2Ground: Same as video stream, ```k=6 n=12``` by default).

It can be used for downlink telemetry (Mavlink 1, Mavlnk2, LTE) and RC.

Setup baudrate 115200 for the UARTs.

## Mavlink 2 RC

The **hx-esp32-cam-fpv** system supports remote control via the **Mavlink 2** protocol. It accepts **Mavlink 2 RC command messages** (```MAXLINK_RC_CHANNELS_OVERRIDE```) over the VRX UART interface.

Although **Mavlink 1** and even **MSP RC** are also compatible, the system is specifically optimized for **Mavlink 2**. It accurately detects the boundaries of RC packets and transmits them without aggregation to minimize latency.

Example setup with https://github.com/RomanLut/hx_espnow_rc TX/RX modules:

![alt text](/doc/images/mavlink2_rc.png "mavlink2_rc")

By default, on **Radxa** or **Runcam VRX**, stream is sent using **USB serial** if present, otherwise **UART3**. Port can be selected in **GS Settings->Wifi Settings** menu.

## MSP RC translation (Mavlink2MspRC)

Some flight controllers have a limited number of available UART ports.

To address this, you can enable a camera configuration option that translates **Mavlink 2 RC commands** (```MAXLINK_RC_CHANNELS_OVERRIDE```) into **MSP RC commands** (```MSP_SET_RAW_RC```). These translated commands are then sent over the **DisplayPort OSD UART**, allowing full aircraft control without requiring a Mavlink UART connection to the flight controller. This is supported by INAV firmware.

*Note: Translating MSP telemetry to Mavlink telemetry is currently not implemented*.

## Disabling camera from RC Controller

If **Mavlink RC** is used, it is possible to disable camera using channel configured in ```Camera Stop Channel``` camera configuration.
