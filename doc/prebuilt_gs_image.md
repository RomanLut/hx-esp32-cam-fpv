
# Preparing SD Card image for GS from prebuilt image

* Download prebuilt image from release https://github.com/RomanLut/hx-esp32-cam-fpv/releases  (choose correct image - for RPI or Radxa Zero 3W)

* Write image to SD Card using **Raspberry Pi Imager** https://www.raspberrypi.com/software/
 
   *Choose "Custom OS"...*

* Let it boot once into GS software or user prompt.
  
   *Sometimes it can boot into user prompt on very first boot because wlan0 and wlan1 get allocated in incorrect order. If it happends - just reboot, it should correctly boot into GS software.*

* If dualboot image is unable to load correct drivers for wifi cards, boot into **RubyFPV** ( by holding **REC GS** button on button) and select **Factory Reset** in **RubyFPV** menu.

