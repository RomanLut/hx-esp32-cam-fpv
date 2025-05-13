
DEPRECATED: Currently GS software is build based on **RubyFPV** images.

# Building Ground Station image for Raspberry Pi

This process is tested on Raspberry Pi Zero 2W and Raspberry Pi 4B 2GB. Other in-beetween models should also work. Raspberry Pi Zero W and 1 are not supported (too low performance).

Image can be prepared on Raspberry PI 4B and used on Raspberry PI Zero 2W, except rtl8812au driver installation. Driver, compiled on RPI2W does not work on PRI4 and vice versa. You have to repeat driver installation steps. Once compiled on both boards, image works on both.

*On Raspberry Pi Zero 2W, due to low memory, you may want to set GPU Memory to 16 before building anything. You may need to use ```make -j1``` instead of ```make -j4```. Set GPU Memory to 64 after last step.*

Driver for AR9271 wifi card is included in the OS image and works without additional setup.

* Download distribution of Rapberri Pi OS (Buster 32bit) with 5.10.17-v7+ kernel:
https://downloads.raspberrypi.org/raspios_lite_armhf/images/raspios_lite_armhf-2021-05-28/

* Write to SD card using Raspberry PI Imager. In the tool, provide credentials to your wifi network if you want to use ssh over wifi for the setup. Alternativelly, you can connect PI to network using ethernet. If you do not have usb keyboard, make sure to enable SSH in services. You also have to change default login to enable SSH. https://www.raspberrypi.com/software/

* Boot image. Default credentials: ```user: pi``` ```password: raspberry``` (you may have changed this in the tool). Default credentials for prebilt image: ```pi``` ```1234```.

* Either use connected usb keyboard or ssh connect using putty. Find out ip address: ```ifconfig```

  If still not connected to internet, run ''sudo raspi-config''' and setup wifi network: **System Options -> Wireless LAN.**

* Update to the latest kernel and reboot:

  ```sudo apt-get update```

  ```sudo apt-get upgrade -y```

  ```sudo reboot```

* Check kernel version: ```uname -r``` Should be: ```5.10.103-v7l+```

* start ```sudo raspi-config``` and change the following options:
  * **Display Options -> Resolution -> 1280x720x60Hz**
  * **Interface options -> Serial Port -> Shell: No, Hardware enable: Yes**
  * **[For Raspberry PI Zero 2W]: Advanced options -> GL Driver -> Fake KMS**
  * **[For Raspberry PI Zero 2W]: Advanced options -> Compositor -> disable compositor**
  
Save and reboot.

* Remove black border from screen:

  ```sudo nano /boot/config.txt```

  Uncomment:

  ```#disable_overscan=1```

  Exit and save (Ctrl+X).

* Install required packages: 

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev raspberrypi-kernel-headers dkms git aircrack-ng```

* Install and compile SDL library. We have to build SDL library to run application without desktop environment.
 
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

  ```git config --global http.postBuffer 350000000``` (For Raspberry PI Zero 2W)
  
  ```git clone -b v5.2.20-rssi-fix-but-sometimes-crash https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Download **esp32-cam-fpv** repository:
 
  ```cd /home/pi/```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```
  
* Adding GPIO keys support:
 
  ```sudo nano /boot/config.txt```
  
  Add a line: ```gpio=24,18,22,27,23,17,4=pd```
  
  Save and exit (Ctrl+X)
  
* Build ground station software:

  ```cd /home/pi/```

    ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check that everything works:

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh```

  (exit using SPACE)

* [Optional] Pair bluetooth mouse to be able to access OSD menu or debug interface with it:
  * ```bluetoothctl```
  * ```scan on```
  * put mouse into pairing mode, find out address
  * ```pair 12:23:34:45:56:67```
  * ```quit```
  * ```sudo reboot```

* Add ground station software to autolaunch:

  ```sudo nano /etc/rc.local```

  Add a line before before ```exit 0```

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh &```

  Exit and save (Ctrl+X):

* Reboot, check if everything works:

  ```sudo reboot```

# Updating groundstation image

Groundstation software is started automatically. Once it is started, wifi connection is disabled. To be able to reconfigure image, unplug external wifi card. Image will boot and will keep connection to access point using internal wifi card.

To update groundstation software, pull updates from '''release''' branch:

  ```cd esp32-cam-fpv```
  
  ```cd gs```
  
  ```git pull```
  
  ```make```


# Building Ground Station development image for Raspberry Pi

Development image is based on desktop environment. Raspberry Pi 4B 2-4GB is recommented for development. 

* Download distribution of Rapberri Pi OS (Buster 32bit) with 5.10.17-v7+ kernel:
https://downloads.raspberrypi.org/raspios_armhf/images/raspios_armhf-2021-05-28/

* Write to SD card using Raspberry PI Imager https://www.raspberrypi.com/software/

* Boot image. Finish configuration wizard: change password, connect to wifi, check "Screen has black border" checkbox, **make sure to update software**. Reboot.

* Open terminal. Check kernel version: ```uname -r``` Should be: ```5.10.103-v7l+```

* Disable screen blanking: Preferences -> Raspberry Pi Configuration -> Display -> Screen blanking: Disabled

* Change screen resolution: Preferences -> Screen configuration -> 1280x720

* start ```sudo raspi-config``` and change the following options:
  * **Display Options -> Resolution -> 1280x720x60Hz**
  * **Interface options -> Serial Port -> Shell: No, Hardware enable: Yes**
  * **Advanced options -> Compositor -> Disable**
  * [Raspberry Pi Zero 2W] **Advanced options -> GL Driver -> G3 GL (Full KMS) OpenGL desktop driver with full KMS**
  
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

  ```git clone -b v5.2.20-rssi-fix-but-sometimes-crash https://github.com/svpcom/rtl8812au/```
 
  ```git config --global http.postBuffer 350000000``` (For Raspberry PI Zero 2W)

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Download **esp32-cam-fpv** repository:

  Clone eiither stable version (release branch):

  ```cd /home/pi/```

  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

  Or unstable development version (master branch):

  ```git clone --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Adding GPIO keys support:
 
  ```sudo nano /boot/config.txt```
  
  Add a line: ```gpio=24,18,22,27,23,17,4=pd```
  
* Build ground station software:

  ```cd /home/pi/```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check that everything works:

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh```

  (exit using SPACE)
  

# References

* Build SDL to run without X11: https://lektiondestages.art.blog/2020/03/18/compiling-sdl2-image-mixer-ttf-for-the-raspberry-pi-without-x11/

* Build esp32-cam-fpv original project docs: https://github.com/jeanlemotan/esp32-cam-fpv/blob/main/README.md

* AddX11 app to autostart: https://learn.sparkfun.com/tutorials/how-to-run-a-raspberry-pi-program-on-startup/method-2-autostart

* Visual Studio Code Remote development https://www.youtube.com/watch?v=Lt65Z30JcrI

* Pair bluetooth device from command prompt: https://bluedot.readthedocs.io/en/latest/pairpipi.html#using-the-command-line



