
# Making image file from SD card for release
- Build image on PRI4 https://github.com/RomanLut/hx-esp32-cam-fpv/blob/master/doc/building_gs_image.md on 32GB SD Card
 
- Insert SD card into PRI 2W and compile rtl8812au driver.

- * start ```sudo raspi-config``` and change the following options:
  * **Advanced options -> GL Driver -> Fake KMS**
  * **Advanced options -> Compositor -> disable compositor**

- Insert SD card and 64GB  Flash drive into RPI4.

- Install modified pishrink.sh script and copy it to the ```/usr/local/bin``` folder by typing: 

```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/master/scripts/pishrink.sh```

```sudo chmod +x pishrink.sh```

```sudo mv pishrink.sh /usr/local/bin```

- Check the mount point path of your USB drive by entering:

```lsblk```

- Insert NTFS formatted USB drive

- Mount usbdrive:

```sudo mkdir -p /mnt/usb1```

```sudo mount /dev/sda1 /mnt/usb1```

- Create image from SD card to USB drive:

```sudo dd if=/dev/mmcblk0 of=/mnt/usb1/espvrx.img bs=1M```

```cd /mnt/usb1```

```sudo pishrink.sh -z -a espvrx.img```

```sudo umount /mnt/usb1```

# References

How to Back Up Your Raspberry Pi as a Disk Image https://www.tomshardware.com/how-to/back-up-raspberry-pi-as-disk-image
