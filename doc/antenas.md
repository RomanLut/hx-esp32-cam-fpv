# Antenas

This **2.4Ghz** antena seems to be the best choice for the UAV because it is flexible and can be mouted on the wing using part of cable tie or toothpick:

![alt text](/doc/images/2dbi_dipole.jpg "2dbi dipole")

Various PCB antenas for **2.4Ghz** can be considered (not tested):

![alt text](/doc/images/pcb_antena.jpg "pcb antena")

The best choice for GS is 4x5dBi dipoles or 2x5dbi dipoles + 2xBetaFPV Moxon Antenna.

It is important that all antenas should be mounded **VERTICALLY**.

**esp32cam** PCB antena can not provide range more then a few metters. 

Note: **esp32cam** board requires soldering resistor to use external antena: https://www.youtube.com/watch?v=aBTZuvg5sM8

Do not power wifi card or **ESP32** without antena attached; it can damage output amplifier.

Do not use **5.8Ghz** channels with **2.4Ghz** antennas attached and vise versa; it can damage output amplifier.
