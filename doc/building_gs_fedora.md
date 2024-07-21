# Running Ground Station software on Fedora Workstation

This instruction describes steps for running Ground Station on Fedora Workstation (f.e. on old x86_64 notebook).

External Wifi card which supports monitor mode and injection is still required (rtl8812ua, ar9271). 

Internal wifi card may work or may not. Intel 6300 AGN card does not work for me; even though it works on Ubuntu.

* Install required packages:

   ```sudo yum install SDL2-devel turbojpeg-devel freetype-devel libpcap-devel dkms aircrack-ng```

   ```sudo dnf install "Development Tools" "C Development Tools and libraries" ```

* Install [rtl8812au driver](https://github.com/svpcom/rtl8812au/).

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

   Kill NeworkManager:
  
   ```sudo airmon-ng check kill```

   OR request NetworkManager to do not manage your adapter:

   ```nmcli dev set wlp3s0 managed no```

   Switch interface to monitor mode:

  ```sudo airmon-ng start wlp3s0```

     (on this step interface may be renamed to wlan0mon. If it does, use wlan0mon in the next steps)
  
   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1```

* If it prints "Interface does not support monitor mode", try with  ```-sm 1``` parameter:

   ```sudo ./gs -rx wlp3s0 -tx wlp3s0 -fullscreen 1 -sm 1```

Use ```./gs -help``` to see available command line parameters.
