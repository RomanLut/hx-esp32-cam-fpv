
# Making image file from SD card for release
- Build image on PRI4 https://github.com/RomanLut/hx-esp32-cam-fpv/blob/master/doc/building_gs_image.md on **32GB** SD Card
 
- Insert SD card into PRI 2W and compile rtl8812au driver.

- * start ```sudo raspi-config``` and change the following options:
  * **Advanced options -> GL Driver -> Fake KMS**
  * **Advanced options -> Compositor -> disable compositor**

- Insert SD card into PRI4 or PRI2W.

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

```sudo dd if=/dev/mmcblk0 of=/mnt/usb1/espvrx.img bs=1M```

```cd /mnt/usb1```

```sudo pishrink.sh -z -a espvrx.img```

```cd ~```

```sudo umount /mnt/usb1```

# References

How to Back Up Your Raspberry Pi as a Disk Image https://www.tomshardware.com/how-to/back-up-raspberry-pi-as-disk-image
