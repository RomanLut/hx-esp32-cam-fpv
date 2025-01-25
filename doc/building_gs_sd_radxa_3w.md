
DEPRECATED: Currently GS images are built based on RubyFPV images.

# Building Ground Station image for Radxa Zero 3W

We use Armbian because it provides OpenGL ES harware acceleration with KMSDRM without desktop (contrary to official Debian image for Radxa Zero 3W).

We are using 22.04 with core 5.10.* because wifi driver require it.

* Download distribution of Armbian, Ubuntu 22.04 (Jammy) Minimal/IOT, kernel 5.10.
https://www.armbian.com/radxa-zero-3/

UPDATE: Armiban Ubuntu minimum 22.04 image is not available at Armbian site anymore. There seems to be 22.04 version here, https://joshua-riek.github.io/ubuntu-rockchip-download/boards/radxa-zero3.html , but it is not checked.

* Write to SD card using Raspberry PI Imager (select **Other Os**).

* Boot image.

* Either use connected usb keyboard or ssh connect using putty. Find out ip address: ```ifconfig```

* Update:

  ```sudo apt-get update```

* start ```sudo armbian-config``` and change the following options:
 * Software -> Install headers
 * **TODO**: enable UART
  
* Install required packages: 

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms git aircrack-ng```

* Install rtl8812au driver:

  ```cd ~```

  ```git clone -b v5.2.20-rssi-fix-but-sometimes-crash https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Download **esp32-cam-fpv** repository:
 
  ```cd ~```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```
  
* Adding GPIO keys support:
 
  **TODO**

* Build ground station software:

  ```cd ~```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check that everything works:

  ```sudo /home/radxa/esp32-cam-fpv/gs/launch.sh```

  (exit using SPACE)

* Add ground station software to autolaunch:

  ```sudo nano /etc/rc.local```

  Add a line before before ```exit 0```

  ```sudo /home/pi/esp32-cam-fpv/gs/launch.sh```

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



