# Radxa Zero 3W/3E Ground Station

Preparing SD Card for **Radxa Zero 3W** GS: [/doc/software_for_radxa.md](/doc/software_for_radxa.md)

Note: Joystick and keys wiring is compatible with **RubyFPV**. GS built for **RubyFPV** can be used with **hx-esp32cam-fpv** at the same time with dualboot SD Card.

Hold **Air Rec** button on powerup to boot **hx-esp32-cam-fpv** software. Hold **GS Rec** button on powerup to boot **RubyFPV** software. If no buttons are pressed, last software is loaded on reboot.

STL files for 3D printing **Radxa Zero 3W** GS enclosure on Thingiverse: https://www.thingiverse.com/thing:6847533

![alt text](/doc/images/radxa3w_gs1.jpg "radxa3w_gs1.jpg")
![alt text](/doc/images/radxa3w_gs2.jpg "radxa3w_gs2.jpg")
![alt text](/doc/images/radxa3w_gs3.jpg "radxa3w_gs3.jpg")
![alt text](/doc/images/radxa3w_gs4.jpg "radxa3w_gs4.jpg")
![alt text](/doc/images/radxa3w_gs5.jpg "radxa3w_gs5.jpg")
![alt text](/doc/images/gs_pinout_radxa.png "gs_pinout_radxa")

It is highly recommended to install cooling fan [/doc/connecting_fan.md](/doc/connecting_fan.md)

Radxa Zero 3W pinout for reference: [https://docs.radxa.com/en/zero/zero3/hardware-design/hardware-interface](https://docs.radxa.com/en/zero/zero3/hardware-design/hardware-interface)

**Bill of materials:**

| Position | Name                             | Quantity |
|----------|----------------------------------|----------|
| 1        | Radxa Zero 3W                    | 1        |
| 3        | DC-DC 5V 2A                      | 1        |
| 3        | DC Female Connector              | 1        |
| 4        | USB Type C OTG Male Connector    | 1        |
| 5        | USB 2.0 HUB                      | 1        |
| 6        | RTL 8812AU                       | 2        |
| 7        | U.fl to SMA Coax Cable           | 4        |
| 8        | 2.4Ghz+5.8Ghz Dipole Antena      | 4        |
| 9        | 4-Position Joystick              | 1        |
| 10       | Button                           | 2        |
| 11       | Resistor 1kOhm                   | 1        |
| 12       | 25x25x8mm Fan                    | 1        |
| 13       | Heatsink                         | 1        |
| 14       | 8+GB MicroSD Class A1 Card       | 1        |
| 15       | Fan PWM Control board (optional) | 1        |
| 16       | USB Female Connector (optional)  | 2        |
| 17       | USB-LAN adapter*                 | 1        |

*USB-LAN adapter is required for software installation.*
