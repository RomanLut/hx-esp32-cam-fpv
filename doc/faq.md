# FAQ

* Can original **Raspberry Pi Zero W** be used as GS?
  
  No, RPI0W does not have enough performance to decode 800x600 MJPEG stream with it's CPU. Moreveover, **RPI Zero 2W** support will be probably dropped soon. Recommended air unit is **Radxa Zero 3W/E**.

* Do I need to pair Air unit and GS in **Raw Broadcast mode**?

  No, the ground station (GS) will automatically connect to any unpaired air unit detected on the selected Wi-Fi channel. Once connected, the air unit will communicate exclusively with that GS until it is rebooted. While multiple air unit/GS pairs can technically operate on the same channel, this is not recommended.

* What if packet lost and FEC can not recover?

  Then the whole frame is lost. That's why FEC is set to high redundancy by default.
