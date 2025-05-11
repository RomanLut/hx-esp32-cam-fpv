# Adding hx-esp32-cam-fpv GS software to existing RubyFPV SD Card using script

* Connect RPI GS to LAN using USB-LAN adapter

* Boot **RubyFPV** image

* Enable ssh in Ruby interface **System\Network\Enable SSH**

* ssh to running **RubyFPV** GS. Credentials are ```pi/raspberry```

* ```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/refs/heads/master/scripts/install_on_ruby.sh```

* ```chmod +x install_on_ruby.sh```

* ```./install_on_ruby.sh```

* Wait until script finishes and reboots system to **hx-esp32-cam-fpv** GS software.

Note that installation process may take ~1h on **Raspberry Pi 2W**. ssh connection should stay alive untill reboot.

See also: Installing fan control service [/doc/installing_fan_control_service.md  ](/doc/installing_fan_control_service.md  ) 

> [!NOTE]
> *Known problem: With Wifi drivers installed in RubyFPV image currently, esp32-cam-fpv GS software is unable to show RSSI*

# Manually adding hx-esp32-cam-fpv GS software to exiting RubyFPV SD Card

  The following steps describe what ```install_on_ruby.sh``` script does automatically.

* Download lastest **RubyFPV** image for Raspberry Pi: https://rubyfpv.com/downloads.php

* Write to SD card using **Raspberry PI Imager** (select **Other Os**).

* Connect **Raspberry PI** GS to network using USB-LAN adapter

* Boot image on RPI GS. Wait untill **RubyFPV** interface boots fully.

* ssh to RPI GS. Credentials are ```pi/raspberry```

* Actualise time:

  ```sudo timedatectl set-ntp true```

* Install required packages: 

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms git aircrack-ng```

* Install and compile SDL library.

  ```cd ~```
 
  ```wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz```

  ```tar zxf SDL2-2.0.18.tar.gz```

  ```rm SDL2-2.0.18.tar.gz```

  ```cd SDL2-2.0.18```

  ```./autogen.sh```

  ```./configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl```

  ```make -j4```

  ```sudo make install```

* Download **esp32-cam-fpv** repository:
 
  ```cd ~```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Build ground station software:

  ```cd ~```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Modify launch script:

  ```sudo nano /root/.profile```
  
  Comment out all lines starting from ```echo "Launching..."```

  Add line: ```/home/pi/esp32-cam-fpv/scripts/boot_selection.sh``` 

* Save and reboot:

  ``` sudo reboot ```



# Updating groundstation image

* Connect RPI GS to network using USB-LAN adapter

* To update groundstation software, pull updates from '''release''' branch:

  ```cd esp32-cam-fpv```
  
  ```cd gs```
  
  ```git pull```
  
  ```make```

# Notes

* LAN is not initialized while GS software is running. Exit GS software to initialize LAN. 

* If image is booted on Raspberry 4 once, it will not boot on Raspberry Pi Zero 2W anymore.
 

# Development

 See notes on development with **RubyFPV** based image: [/doc/vs_code_remote_development.md  ](/doc/vs_code_remote_development.md  ) 
