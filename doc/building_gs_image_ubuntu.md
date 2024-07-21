
# Running Ground station onUbuntu desktop

This instruction describes steps for running Ground Station on Ubuntu desktop (f.e. old notebook or Raspberry Pi).

It starts from building live USB flash image. If you want to run image onexisting system, just skip first steps.

External Wifi card which supports monitor mode and injection is still required (rtl8812ua, ar9271). 

Internal wifi card may work or may not. It works for me with Intel 6257 card.

* Download Ubuntu Desktop image https://ubuntu.com/download/desktop

* Write image to USB stick 
 
   Use Rufus on Windows https://rufus.ie/ 

   Set "Persistent partition size" at least 1Gb to enable persistent storage on Live USB image.

   Detailed description: https://itsfoss.com/ubuntu-persistent-live-usb/

* Boot from USB

* Install rtl8812au driver:

  ```cd /home/pi/```

  ```git config --global http.postBuffer 350000000``` (For Raspberry PI Zero 2W)
  
  ```git clone -b v5.2.20-rssi-fix-but-sometimes-crash https://github.com/svpcom/rtl8812au/```

  ```cd rtl8812au```

  ```sudo ./dkms-install.sh```

* Install required packages: '''sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev dkms git aircrack-ng'''

* Install and compile SDL library. We have to build library to run application without desktop.
 
  ```wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz```

  ```tar zxf SDL2-2.0.18.tar.gz```

  ```rm SDL2-2.0.18.tar.gz```

  ```cd SDL2-2.0.18```

  ```./autogen.sh```

  ```./configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl```

  ```make -j4```

  ```sudo make install```

* Download **esp32-cam-fpv** repository:
 
  ```cd /home/pi/```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Build ground station software:

  ```cd /home/pi/```

    ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```


**TODO**