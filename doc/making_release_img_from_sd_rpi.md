
# Making image file from SD card for release
- use RPI2W with USB-LAN adapter to be able to ssh (pi/1234)

- Build the dual-boot image on Raspberry Pi 4 by following
  [Adding GS software to a RubyFPV Raspberry Pi SD card](/doc/adding_gs_software_to_ruby_sd_rpi.md).
  Use an **8 GB** (recommended), **16 GB**, or **32 GB** SD card.
 
- If the release image must also support Raspberry Pi Zero 2W, boot the same
  card there and build the pinned patched RTL8812AU module for that running
  kernel:

  ```bash
  cd ~/esp32-cam-fpv
  MAKE_JOBS=1 bash scripts/install_ruby_rtl8812au_driver.sh
  sudo reboot
  ```

  Verify that `88XXau_wfb` loads on both Raspberry Pi 4 and Zero 2W before
  creating the release image. Each platform uses its own kernel module and Ruby
  packaged-driver copy.

- set default GS settings before doing next steps

- * start ```sudo raspi-config``` and change the following options:
  * **Advanced options -> GL Driver -> Fake KMS**
  * **Advanced options -> Compositor -> disable compositor**

- check that credentials are not used:

    ```cd ~/esp32-cam-fpv/```

    ```git config --show-origin credential.helper``` should be empty.

    ```git remote -v``` should not show credentials in url.

- Delete GS recordings:

    ```cd ~/esp32-cam-fpv/gs/```

    ```rm *.avi``` 

- Install modified pishrink.sh script and copy it to the ```/usr/local/bin``` folder by typing: 

```wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/release/scripts/pishrink.sh```

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

```sudo dd if=/dev/mmcblk0 of=/mnt/usb1/espvrx_rpi.img bs=1M status=progress```

```sudo pishrink.sh -z -a /mnt/usb1/espvrx_rpi.img```

```sudo umount /mnt/usb1```

# Using script

`scripts/make_release_img_from_sd_rpi.sh` performs the image-copy and shrink
steps automatically after the SD card has been prepared and verified.


# References

How to Back Up Your Raspberry Pi as a Disk Image https://www.tomshardware.com/how-to/back-up-raspberry-pi-as-disk-image
