# Bidirectional serial connection

There is bidirectional stream sent with FEC encoding (Ground2Air: ```k=2 n=3```, Air2Ground: Same as video stream, ```k=6 n=12``` by default).

It can be used for downlink telemetry (Mavlink 1, Mavlnk2, LTE) and RC.

Setup baudrate 115200 for the UARTs.

## Mavlink 2 RC

The **hx-esp32-cam-fpv** system supports remote control via the **Mavlink 2** protocol. It accepts **Mavlink 2 RC command messages** (```MAXLINK_RC_CHANNELS_OVERRIDE```) over the VRX UART interface.

Although **Mavlink 1** and other telemetry protocols can be used, the system is specifically optimized for **Mavlink 2**. It accurately detects the boundaries of RC packets and transmits them without aggregation to minimize latency.

Example setup with https://github.com/RomanLut/hx_espnow_rc TX/RX modules:

![alt text](/doc/images/mavlink2_rc.png "mavlink2_rc")

By default, on **Radxa** or **Runcam VRX**, stream is sent using **USB serial** if present, otherwise **UART3**. Port can be selected in **GS Settings->Wifi Settings** menu.

### RADIO_STATUS and INAV OSD setup

The camera also injects a Mavlink ```RADIO_STATUS``` message once per second into the stream sent to the flight controller. Injection is enabled only when ```Mavlink2MspRC``` is disabled; when RC commands are translated to MSP, the equivalent link status is sent using MSP instead. While Mavlink data is flowing, the camera inserts ```RADIO_STATUS``` only after a complete Mavlink message boundary. 

For INAV 9, use the following CLI settings so ```RADIO_STATUS``` is decoded using separate RSSI dBm and link-quality fields:

```
set receiver_type = SERIAL
set serialrx_provider = MAVLINK
set mavlink_version = 2
set mavlink_radio_type = ELRS
set osd_crsf_lq_format = TYPE1
save
```

```mavlink_radio_type = ELRS``` does not change the serial protocol to CRSF or require an ELRS receiver. The connection remains Mavlink; this option only selects how INAV interprets the fields in ```RADIO_STATUS```.

Enable these INAV OSD elements:

* **RSSI dBm** displays ```rxLinkStatistics.uplinkRSSI```, such as ```-57 dBm```.
* **Uplink Link Quality** displays ```rxLinkStatistics.uplinkLQ``` as a value from 0 to 100. ```TYPE1``` displays the plain numeric value.

Do not use **RSSI Value** indicator. 

## Baudrate

 Baudrate for Mavlink UART can be configured in **OTA Mode** and in ``Menu->Camera..-RC..`` menu.

## MSP RC translation (Mavlink2MspRC)

Some flight controllers have a limited number of available UART ports.

To address this, you can enable a camera configuration option that translates **Mavlink 2 RC commands** (```MAXLINK_RC_CHANNELS_OVERRIDE```) into **MSP RC commands** (```MSP_SET_RAW_RC```). These translated commands are then sent over the **DisplayPort OSD UART**, allowing full aircraft control without requiring a Mavlink UART connection to the flight controller. This is supported by INAV firmware.

*Note: Translating MSP telemetry to Mavlink telemetry is currently not implemented*.

## Disabling camera from RC Controller

If **Mavlink RC** is used, it is possible to disable camera using channel configured in ```Camera Stop Channel``` camera configuration.
