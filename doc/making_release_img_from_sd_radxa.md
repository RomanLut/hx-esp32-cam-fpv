
# Making image file from SD card for release (Radxa Zero 3W)

- use radxa with USB-LAN adapter to be able to ssh (radxa/radxa)

- create SD Card with Dualboot RubyFPV Image [/doc/adding_gs_software_to_ruby_sd_radxa3.md](/doc/adding_gs_software_to_ruby_sd_radxa3.md) on **8GB** SD Card
  or use previous release. In previous release, update RubyFPV using it's interface.

- install fan control service [/doc/installing_fan_control_service.md ](/doc/installing_fan_control_service.md)

- boot RubyFPV to install drivers

- test **hx-esp32-cam-gs** software

- set default GS settings before doing next steps

- boot **hx-esp32-cam-gs** software

- exit to shell

* Actualise time to solve 'cetificate is not trusted' errors:

  ```sudo timedatectl set-ntp true```

- check that credentials are not used:

    ```cd ~/esp32-cam-fpv/```

    ```git config --show-origin credential.helper``` should be empty.

    ```git remote -v``` should not show credentials in url.

- Delete GS recordings:

    ```cd ~/esp32-cam-fpv/gs/```

    ```rm *.avi```

- zero free space to make compressed image smaller:

  ```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/release/scripts/zero_free_space.sh```

  ```sudo chmod +x zero_free_space.sh```

  ```./zero_free_space.sh```

- Check the mount point path of your USB drive by entering:

  ```lsblk```

- Insert **16+GB** Flash drive formatted to **NTFS**. Flash drive should have enough free space for **8GB** SD Card image.

- Mount usb flash drive:

  ```sudo mkdir -p /mnt/usb1```

  ```sudo mount /dev/sda1 /mnt/usb1``` 

  _(note that it could be ```/dev/sdb1``` depending on USB port used)_

- force filesystem resize on first boot:

  ```sudo rm /etc/growroot-grown```
  
  ```sudo rm /etc/resize2fs-done```

- remove boot count file to force system to resize filesystem and install drivers on the first boot:
  
   ```rm /home/radxa/ruby/config/boot_count.cfg```
   
- Create image from SD card to USB drive:

  ```sudo dd if=/dev/mmcblk1 of=/mnt/usb1/espvrx_dualboot_radxa3w.img bs=1M status=progress```

  ```sudo umount /mnt/usb1```

  Compress .img file on PC.
