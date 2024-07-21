# Running Ground Station software on Fedora Workstation

This instruction describes steps for running Ground Station on Fedora Workstation (f.e. on old x86_64 notebook).

It starts from building live USB flash image. If you want to run image on the existing system, just skip first steps.

External Wifi card which supports monitor mode and injection is still required (rtl8812ua, ar9271). 

Internal wifi card may work or may not. It works for me with Intel 6257 card.

* Download Fedora Workstation 40 Live ISO image https://fedoraproject.org/en/workstation/download 

* Write image to USB stick. Use at least 8GB flash drive to be able to allocate at least 2GB for persistent storage.
 
   Use Rufus on Windows https://rufus.ie/ 

   Set "Persistent partition size" at least 2Gb to enable persistent storage on Live USB image.

   Steps are similar to Ubuntu USB stick creation: https://itsfoss.com/ubuntu-persistent-live-usb/

* Boot from USB stick, Select "Try this image"

* Setup wifi connection to internet

* Install required packages: ```sudo apt install --no-install-recommends -y git libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev dkms git aircrack-ng```

* Install and compile SDL library.
 
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

  ```sudo airmon-ng start wlp3s0```

     (on this step interface may be renamed to wlan0mon. If it does, use wlan0mon in the next steps)
  
   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1```

* If it prints "Interface does not support monitor mode", try with  ```-sm 1``` parameter:

   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1 -sm 1```

Use ```./gs -help``` to see available command line parameters.









We will assume you are using a dedicated RTL8812AU WiFi adapter on interface `wlp0s20f0u1`.

* Install [rtl8812au driver](https://github.com/svpcom/rtl8812au/).
* Request NetworkManager to do not manage your adapter:
```
nmcli dev set wlp0s20f0u1 managed no
```
* Install dependencies:
```
sudo yum install SDL2-devel turbojpeg-devel freetype-devel
```
* Download the GS code:
```
git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv
```
* Build the app:
```
cd esp32-cam-fpv/gs
make -j4
```
* Launch GS:
```
sudo ./gs -rx wlp0s20f0u1 -tx wlp0s20f0u1
```
