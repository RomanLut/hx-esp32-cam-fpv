
# Running Ground Station software on Ubuntu desktop

This instruction describes steps for running Ground Station software on Ubuntu desktop (f.e. old notebook or Raspberry Pi).

For notebook, it starts from building live USB flash drive. If you want to run image on the existing system, just skip first steps.

For Raspberry Pi, follow these steps https://ubuntu.com/tutorials/how-to-install-ubuntu-desktop-on-raspberry-pi-4#1-overview to prepare Ubuntu SD card. Then jump to "Install required packages" step.

External Wifi card which supports monitor mode and injection is still required (rtl8812ua, ar9271). 

Internal wifi card may work or may not. It works for me with Intel 6257 card.

* Download Ubuntu Desktop image https://ubuntu.com/download/desktop

* Write image to USB stick. Use at least 12GB flash drive to be able to allocate at least 2GB for persistent storage.
 
   Use Rufus on Windows https://rufus.ie/ 

   Set "Persistent partition size" at least 1Gb to enable persistent storage on Live USB image.

   Detailed description: https://itsfoss.com/ubuntu-persistent-live-usb/

* Boot from USB stick

* On Notebook: Select **Try Ubuntu**

* Pass initial Ubuntu configuration, setup wifi connection to internet

* Install required packages: ```sudo apt install --no-install-recommends -y git libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev dkms git aircrack-ng```

* Install and compile SDL library. We have to build library to run application without desktop.
 
  ```wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz```

  ```tar zxf SDL2-2.0.18.tar.gz```

  ```rm SDL2-2.0.18.tar.gz```

  ```cd SDL2-2.0.18```

  ```./autogen.sh```

  ```./configure```

  ```make -j4```

  ```sudo make install```

* Install rtl8812au driver:

  ```cd ~```

  ```git config --global http.postBuffer 350000000``` (For Raspberry PI Zero 2W)
  
  ```git clone -b v5.2.20-rssi-fix-but-sometimes-crash https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Download **esp32-cam-fpv** repository:
 
  ```cd ~```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Build ground station software:

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check name of Wifi card interface:

  ```sudo airmon-ng```

   Note name of Wifi card **Interface**, f.e. **wlp3s0**

* Launch Ground Station software:

   ```sudo airmon-ng check kill```
  
   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1```

* If it prints "Does not support monitor mode", try with  ```-sm 1``` parameter:

   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1 -sm 1```

Use ```./gs -help``` to see available command line parameters.
