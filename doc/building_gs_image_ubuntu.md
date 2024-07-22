
# Running Ground Station software on Ubuntu desktop

This instruction describes steps for running Ground Station software on Ubuntu desktop (f.e. old x86_64 notebook or Raspberry Pi).

For notebook, it starts from building live USB flash drive. If you want to run image on the existing system, just skip first steps untill "Install required packages".

External Wifi card which supports monitor mode and injection is still required (rtl8812ua, ar9271). 

Internal wifi card may work or may not. It works for me with **Intel 6300 AGN card**.

For Raspberry Pi, follow these steps https://ubuntu.com/tutorials/how-to-install-ubuntu-desktop-on-raspberry-pi-4#1-overview to prepare Ubuntu SD card. Then jump to "Install required packages" step.

* Download Ubuntu Desktop image https://ubuntu.com/download/desktop

* Write image to USB stick. Use at least 12GB flash drive to be able to allocate at least 2GB for persistent storage.
 
   Use Rufus on Windows https://rufus.ie/ 

   Set "Persistent partition size" at least 2Gb to enable persistent storage on Live USB image.

   Detailed description: https://itsfoss.com/ubuntu-persistent-live-usb/

* Boot from USB stick, Select **Try Ubuntu**

* Pass initial Ubuntu configuration, setup wifi connection to internet

* Install required packages: ```sudo apt install --no-install-recommends -y git libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms aircrack-ng```

* Install rtl8812au driver:

  ```cd ~```

  ```git clone -b v5.2.20-rssi-fix-but-sometimes-crash https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Download **esp32-cam-fpv** repository:
 
  ```cd ~```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Build ground station software:

  ```cd ~```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Check name of Wifi card interface:

  ```sudo airmon-ng```

   Note name of Wifi card **Interface**, f.e. **wlp3s0**

* Launch Ground Station software:

  Kill NetworkManager:
  
   ```sudo airmon-ng check kill```

   OR request NetworkManager to do not manage your adapter:

   ```nmcli dev set wlp3s0 managed no```

  Switch interface into monitor mode:  

  ```sudo airmon-ng start wlp3s0```

     (on this step interface may be renamed to wlan0mon. If it does, use wlan0mon in the next steps)

  Run GS software:
  
   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1```

* If it prints "Interface does not support monitor mode", try with  ```-sm 1``` parameter:

   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1 -sm 1```

Use ```./gs -help``` to see available command line parameters.
