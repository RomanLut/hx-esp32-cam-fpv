# Raspberry Pi Zero 2W Ground Station

Preparing SD Card for **Raspberry PI GS**: [/doc/software_for_rpi.md](/doc/software_for_rpi.md)

Note: Joystick and keys wiring is compatible with **RubyFPV**. GS built for **RubyFPV** can be used with **hx-esp32-cam-fpv** by swapping SD card.

STL files for 3D printing **Raspberry Pi Zero 2W GS** enclosure on Thingiverse: https://www.thingiverse.com/thing:6624580

## Single rtl8812au variant

Single wifi card is OK for the GS with attached small HDMI monitor.

![alt text](/doc/images/gs_glasses.jpg "gs_glasses")

![alt text](/doc/images/gs_drawing1.jpg "gs_drawing1")

![alt text](/doc/images/gs_drawing2.jpg "gs_drawing2")

![alt text](/doc/images/gs_pinout.png "gs_pinout")

![alt text](/doc/images/gs.jpg "gs")

## Dual rtl8812au variant

Dual wifi cards variant benefits from better reception and less frame dropping.

![alt text](/doc/images/gs2_glasses.jpg "gs2_glasses")

![alt text](/doc/images/gs2_drawing.jpg "gs2_drawing")

![alt text](/doc/images/gs2_wifi_usb.jpg "gs2_wifi_usb")

It is highly recommended to install cooling fan [/doc/connecting_fan.md](/doc/connecting_fan.md)

A small USB 2.0 hub board is used to connect two wifi cards and add two USB port sockets.

Small **rtl8812au** cards are used.

![alt text](/doc/images/gs2_overview.jpg "gs2_overview")

Note that red/black antennas are not recommended unless all you want is to look cool. These are 2dbi wideband antennas. A pair of 2.4Ghz BetaFPV Moxons with 90° adapters are recommended instead.

![alt text](/doc/images/moxon.jpg "moxon")

***Note that Raspberry Pi GS is not actively developed and tested. It might be dropped in future releases.***
