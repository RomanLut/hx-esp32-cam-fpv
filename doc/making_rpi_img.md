
# Making image file from SD card for release

Reference: https://www.tomshardware.com/how-to/back-up-raspberry-pi-as-disk-image

- Install modified pishrink.sh on your Raspberry Pi and copy it to the /usr/local/bin folder by typing: 

```wget https://raw.githubusercontent.com/Drewsif/PiShrink/master/pishrink.sh```
```sudo chmod +x pishrink.sh```
```sudo mv pishrink.sh /usr/local/bin```

- Check the mount point path of your USB drive by entering:

```lsblk```

- Insert NTFS formatted USB drive

- Mount usbdrive:

```sudo mkdir -p /mnt/usb1```
```sudo mount /dev/sda1 /mnt/usb1```

- Create image from SD card to USB drive:

```pi@raspberrypi:~ $ sudo dd if=/dev/mmcblk0 of=/mnt/usb1/espvtx.img bs=1M```

```cd /mnt/usb1```

```sudo pishrink.sh -z -a espvtx.img```

```sudo umount /mnt/usb1```
