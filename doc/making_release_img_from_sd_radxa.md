
# Making image file from SD card for release (Radxa Zero 3W)
- Creade SD Card with Dualboot RubyFPV Image [/doc/adding_gs_software_to_ruby_sd_radxa3.md](/doc/adding_gs_software_to_ruby_sd_radxa3.md) on **8GB**, **16GB** or **32BG** SD Card

- install fan control service [/doc/installing_fan_control_service.md ](/doc/installing_fan_control_service.md)

- Boot **hx-esp32-cam-gs** software

- Exit to shell

- Install modified pishrink.sh script and copy it to the ```/usr/local/bin``` folder by typing: 

  ```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/master/scripts/pishrink.sh```

  ```sudo chmod +x pishrink.sh```

  ```sudo mv pishrink.sh /usr/local/bin```

- Check the mount point path of your USB drive by entering:

  ```lsblk```

- Insert **64GB** Flash drive formatted to NTFS. We use 64GB flash drive, because it should have enought free space for 32GB SD Card image and a shrinked image.

- Mount usbdrive:

  ```sudo mkdir -p /mnt/usb1```

  ```sudo mount /dev/sda1 /mnt/usb1``` 

_(note that it could be ```/dev/sdb1``` depending on USB port used)_

- Create image from SD card to USB drive:

  ```sudo dd if=/dev/mmcblk1 of=/mnt/usb1/espvrx_dualboot_radxa3w.img bs=1M status=progress```

  ```sudo apt-get install pigz```

  ```sudo pishrink.sh -z -a /mnt/usb1/espvrx_dualboot_radxa3w```

  ```sudo umount /mnt/usb1```

# References

How to Back Up Your Raspberry Pi as a Disk Image https://www.tomshardware.com/how-to/back-up-raspberry-pi-as-disk-image
