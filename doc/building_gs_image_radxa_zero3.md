# Building GS image for Radxa Zero 3W

Since release 0.3.1, esp32-cam-fpv images are based on RubyFPV images. 

After adding modifications, user can switch beetween RubyFPV GS or esp32-cam-fpv GS.


* Download lastest RubyFPV image for Radxa Zero 3W: https://rubyfpv.com/downloads.php

* Write to SD card using Raspberry PI Imager (select **Other Os**).

* Connect Radxa GS to network using USB-LAN adapter

* Boot image on Radxa GS.Wait untill Ruby interface boots fully.

* ssh to Radxa GS. Credentials are ```radxa/radxa```

* Actualise time:

  ```sudo timedatectl set-ntp true```

* Install required packages: 

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms git aircrack-ng```

* Download **esp32-cam-fpv** repository:
 
  ```cd ~```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Build ground station software:

  ```cd ~```

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Modify launch script:

 ```sudo nano /root/.profile
 
 Comment out all lines starting from ```echo "Launching Ruby..."```

 Add line: ```/home/radxa/esp32-cam-fpv/scripts/boot_selection.sh``` 

* Save adn reboot:

``` sudo reboot ```



# Updating groundstation image

Connect Radxa GS to network using USB-LAN adapter

To update groundstation software, pull updates from '''release''' branch:

  ```cd esp32-cam-fpv```
  
  ```cd gs```
  
  ```git pull```
  
  ```make```