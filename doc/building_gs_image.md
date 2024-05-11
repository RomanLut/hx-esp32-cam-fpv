# Building ground station image

This process is tested on Raspberry Pi Zero 2W and Raspberry Pi 4B 2GB. Other in-beetween models should also work. Raspberry Pi Zero 0W and 1 are not supported (too low performance).

Image can be prepared on Raspberry PI 4B and used on Raspberry PI Zero 2W, except rtl8812au driver installation. Driver, compiled on RPI2W does not work on PRI4 and vice versa. You have to repeat driver installation steps. Once compiled on both boards, image works on both.

Drivers for AR9271 wifi card are included in OS image and works without additional setup.

* Download distribution of Rapberri Pi OS (Buster 32bit) with 5.10.17-v7+ kernel:
https://downloads.raspberrypi.org/raspios_lite_armhf/images/raspios_lite_armhf-2021-05-28/

* Write to SD card using Raspberry PI Imager. In the tool, provide credentials to your wifi network. Alternativelly, connect PI to network using ethernet. If you do not have usb keyboard, make sure to enable SSH in services. You also have to change default login to enable SSH. https://www.raspberrypi.com/software/

* Boot image. Default credentials: ```user: pi``` ```password: raspberry``` (you may have changed this in the tool)

* Either use connected usb keyboard or ssh connect using putty. Find out ip address: ```ifconfig```

* Update to latest kernel and reboot:

  ```sudo apt-get update```

  ```sudo apt-get upgrade -y```

  ```sudo reboot```

* Check kernel version: ```uname -r``` Should be: ```5.10.103-v7l+```

* start ```sudo raspi-config``` and change the following options:
  * Display Options -> Resolution -> 1280x720x60Hz
  * Interface options -> Serial Port -> Shell: No, Hardware enable: Yes
  * Performance options -> GPU Memory: 128
  
Save and reboot.

* Remove black border from screen:

  ```sudo nano /boot/config.txt```

  Uncomment:

  ```#disable_overscan=1```

  Exit and save (Ctrl+X).

* Install required packages: 

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev raspberrypi-kernel-headers dkms git aircrack-ng```

* Install and compile SDL library. We have to build library to run application without desktop.
 
  ```wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz```

  ```tar zxf SDL2-2.0.18.tar.gz```

  ```rm SDL2-2.0.18.tar.gz```

  ```cd SDL2-2.0.18```

  ```./autogen.sh```

  ```./configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl```

  ```make -j4```

  ```sudo make install```

* Install rtl8812au driver:

  ```cd /home/pi/```

  ```git clone https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Build ground station software:

  ```cd /home/pi/```

  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check that everything works:

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh```

  (exit using SPACE)
  

* Add ground station software to autolaunch:

  ```sudo nano /etc/rc.local```

  Add a line before before ```exit 0```

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh```

  Exit and save (Ctrl+X):

* Reboot, check if everything works:

  ```sudo reboot``



# Building ground station development image

Development image is based on desktop environment. 

* Download distribution of Rapberri Pi OS (Buster 32bit) with 5.10.17-v7+ kernel:
https://downloads.raspberrypi.org/raspios_armhf/images/raspios_armhf-2021-05-28/

* Write to SD card using Raspberry PI Imager https://www.raspberrypi.com/software/

* Boot image. Finish confgiuration wizard: change password, connect to wifi, check "Screen has black border" checkbox, **make sure to update software**. Reboot.

* Open terminal. Check kernel version: ```uname -r``` Should be: ```5.10.103-v7l+```

* Disable screen blanking: Preferences -> Raspberry Pi Configuration -> Display -> Screen blanking: Disabled

* Change screen resolution: Preferences -> Screen configuration -> 1280x720

* start ```sudo raspi-config``` and change the following options:
  * Display Options -> Resolution -> 1280x720x60Hz
  * Interface options -> Serial Port -> Shell: No, Hardware enable: Yes
  * Performance options -> GPU Memory: 128
  * Advanced options -> Compositor -> Disable
  * [Raspberry Pi Zero 2W] Advanced options -> GL Driver -> G3 GL (Full KMS) OpenGL desktop driver with full KMS
  
Save and reboot.

* Install required packages: 

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev raspberrypi-kernel-headers dkms git aircrack-ng```

* Install and compile SDL library.
 
  ```wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz```

  ```tar zxf SDL2-2.0.18.tar.gz```

  ```rm SDL2-2.0.18.tar.gz```

  ```cd SDL2-2.0.18```

  ```./autogen.sh```

  ```./configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl```

  ```make -j4```

  ```sudo make install```

* Install rtl8812au driver:

  ```cd /home/pi/```

  ```git clone https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Build ground station software:

  ```cd /home/pi/```

  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check that everything works:

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh```

  (exit using SPACE)
  

# References

* Build SDL to run without X11: https://lektiondestages.art.blog/2020/03/18/compiling-sdl2-image-mixer-ttf-for-the-raspberry-pi-without-x11/

* Build esp32-cam-fpv origical docs: https://github.com/jeanlemotan/esp32-cam-fpv/blob/main/README.md

* AddX11 app to autostart: https://learn.sparkfun.com/tutorials/how-to-run-a-raspberry-pi-program-on-startup/method-2-autostart

* Visual Studio Code Remote development https://www.youtube.com/watch?v=Lt65Z30JcrI





