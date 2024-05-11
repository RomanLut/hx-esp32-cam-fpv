Building Buster 32bit image for Raspberry Pi Zero W / Zero 2W with monitor mode support on internal wireless card (broadcom 43430,43436,43455)

For Linux kernel 5.10.103-v7+ only!

Whole process will take ~2h.

Installation
-
Download distribution of Rapberri Pi OS (Buster 32bit) with 5.10.17-v7+ kernel:

https://downloads.raspberrypi.org/raspios_armhf/images/raspios_armhf-2021-05-28/
https://downloads.raspberrypi.org/raspios_lite_armhf/images/raspios_lite_armhf-2021-05-28/
https://downloads.raspberrypi.org/raspios_full_armhf/images/raspios_full_armhf-2021-05-28/

Update to kernel 5.10.103-v7+:

```sudo apt-get update && apt-get upgrade```

Check kernel version:

```uname -r```

It should be 5.10.103-v7+. **It will not work for other kernel version.***

Check wifi card model:

```sudo apt-get install aircrack-ng```

```sudo airmon-ng```

It should be ```43430``` or ```43436``` or ```43455```.

Edit ```install.sh```, find section ```Uncomment right version of the card here``` and uncomment ```cd``` command for your card.

This part a little bit misterios for me. It seems that there are multiple versions of firmware, and patches for the different firmwares overwrite the same binary file in the kernel. You have to build only correct one.
```bcm43430a1/7_45_41_46/``` worked for me.

Current firmware version can be seen using:
```dmesg | grep brcmfmac```
You probably have to choose closest lower of higher version.


```./install.sh```

During installation, answer Yes to to install kernel headers.

Check log if everything is went correctly.

```reboot```.


modinfo -F filename brcmfmac
xz /lib/modules/5.10.103-v7+/kernel/drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko

Checking
-
```sudo apt-ger install airckrack-ng```

```sudo apt-get install wireshark tcpdump```

```sudo airmon-ng start wlan0```

(ignore error)

```iwconfig```

=> ```wlan0mon``` should appear, started in monitor mode.

```sudo wireshark```

=> use wireshark to capture packets on ```wlan0mon```

or ```tcpdump -i wlan0mon```

If no packets are received, uncomment other folder in script and run again.

Building for other kernel versions
-

Check supported cores versions in https://github.com/seemoo-lab/nexmon/tree/master/patches/driver

For full building instructions check  https://github.com/seemoo-lab/nexmon, this is what install.sh above is doing.
