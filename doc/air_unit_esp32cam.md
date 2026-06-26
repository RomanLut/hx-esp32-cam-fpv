# ESP32-CAM Air Unit

Flashing esp32cam firmware: [/doc/flashing_esp32_cam.md](/doc/flashing_esp32_cam.md)

![alt text](/doc/images/esp32cam_pinout.png "pinout")

The **esp32cam** does not have many free pins. You can optionally solder a **REC button** and an additional **REC status LED** where the **Flash LED** normally is.

![alt text](/doc/images/esp32cam_led.jpg "esp32cam_led.jpg")

Both internal red LED and additional LED are used for indication:

* solid - not recording
* blinking 0.5Hz - recording
* blinking 1Hz - OTA update mode.

**REC button** is used to start/stop air unit recording. Hold **REC button** on powerup to enter OTA update mode.

With PCB antenna, 50m transmission distance can barely be achieved. A jumper has to be soldered to use an external antenna: https://www.youtube.com/watch?v=aBTZuvg5sM8
