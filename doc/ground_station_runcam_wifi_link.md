# Runcam WiFiLink VRX Ground Station

Runcam WiFiLink VRX is built on **rtl8255eu** cards which support **5.8Ghz** band only. It can be used with **esp32c5** air unit only.

**espvrx_dualboot_radxa** image should be used on **Runcam VRX**. Note, that Runcam firmware does not allow booting from SD card. **Runcam VRX** has to be flashed with OpenIPC firmware to unlock SD card boot. Follow **OpenIPC** or **RubyFPV** documentation for flashing.

**Runcam VRX** is using slightly different GPIO buttons wiring, and **Action 2** button is missing. Please select **"GPIO Keys layout: Runcam VRX"** in osd menu. Long press **Action 1 (Air Rec)** button to toggle GS recording.

GS Firmware allows to select any channel from any band, but support depends on hardware. **2.4 GHz** band is enabled by default. To enable **5.8 GHz** band, configure bands in **Ground Station Settings...** OSD menu.

Default channel is set to 7 on **esp32c5** after flashing. **Runcam VRX** will not be able to find it. Boot air unit in [Web interface](/doc/web_interface.md) mode and set channel to 44 in Web interface.

**USB Serial** in **OTG USB port** can be used to transfer Mavlink stream.

![runcam vrx](/doc/images/runcam_vrx.jpg "runcam_vrx")
