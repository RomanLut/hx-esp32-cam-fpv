# Adding hx-esp32-cam-fpv GS software to existing RubyFPV Radxa Zero 3W SD Card using script

* Connect **Radxa Zero 3W** GS to LAN using **USB-LAN adapter**

* Boot **RubyFPV**

* Enable **ssh** in **RubyFPV** interface **Controller Settings\Local Network Settings\Enable SSH**

* ssh to the running **RubyFPV** GS. Credentials are ```radxa/radxa```

* Actualise time:

  ```sudo timedatectl set-ntp true```

* Run ```rsetup```. Enable /dev/ttyS3 overlay: Overlays\Manage Overlays\Enable UART3-M0

* ```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/refs/heads/release/scripts/install_on_ruby.sh```

* ```chmod +x install_on_ruby.sh```

* ```./install_on_ruby.sh```

* Wait until script finishes and reboots system to **hx-esp32-cam-fpv** GS software.

ssh connection should stay alive untill reboot.

See also: Installing fan control service [/doc/installing_fan_control_service.md  ](/doc/installing_fan_control_service.md  ) 

# Manually adding hx-esp32-cam-fpv GS software to existing RubyFPV SD Card

  The following steps describe what ```install_on_ruby.sh``` script does automatically.

* Download lastest **RubyFPV** image for **Radxa Zero 3W**: https://rubyfpv.com/downloads.php

* Write to SD card using **Raspberry PI Imager** (select **Other OS**).

* Connect Radxa GS to network using USB-LAN adapter

* Boot image on Radxa GS. Wait untill Ruby interface boots fully.

* ssh to Radxa GS. Credentials are ```radxa/radxa```

* Actualise time:

  ```sudo timedatectl set-ntp true```

* Install required packages:

  ```sudo apt-get update```

  ```sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms git aircrack-ng```

* Download **esp32-cam-fpv** repository:
 
  ```cd /home/radxa```
 
  ```git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv```

* Build ground station software:

  ```cd esp32-cam-fpv```

  ```cd gs```

  ```make -j4```

* Modify launch script:

  ```sudo nano /root/.profile```
 
  Comment out all lines starting from ```echo "Launching Ruby..."```

  Add line: ```/home/radxa/esp32-cam-fpv/scripts/boot_selection.sh``` 

* Run ```rsetup```. Enable /dev/ttyS3 overlay: Overlays\Manage Overlays\Enable UART3-M0

* Save and reboot:

  ``` sudo reboot ```



# Updating groundstation image

Connect Radxa GS to network using USB-LAN adapter

To update groundstation software, pull updates from '''release''' branch:

  ```cd /home/radxa/```
  
  ```cd esp32-cam-fpv```
 
  ```git pull```
  
  ```cd gs```
  
  ```make```

# Development

 See notes on development with **RubyFPV** based image: [/doc/vs_code_remote_development.md  ](/doc/vs_code_remote_development.md  ) 
