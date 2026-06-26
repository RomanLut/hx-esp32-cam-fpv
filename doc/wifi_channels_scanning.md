# Wifi channels scanning

Wi-Fi Channel Scan is a ground-station diagnostic mode for finding busy or quiet Wi-Fi channels.

When enabled, the GS puts rtl8812au adapter into monitor mode and continuously hops through the channels allowed by the selected Wi-Fi band:

- 2.4 GHz: channels 1-13
- 5.8 GHz: channels 44-165

The result is shown as an OSD bar graph: taller bars mean more Wi-Fi airtime was observed on that channel, so that channel is busier and may be worse for video/control. Shorter or empty bars usually indicate a cleaner channel.

This mode does not connect to the air unit and does not automatically change the configured air/GS link channel. It is only a measurement view to help the user manually choose a better Wi-Fi channel. 

![alt text](/doc/images/wifi_schannels_scan.jpg  "wifi_schannels_scan.jpg")
