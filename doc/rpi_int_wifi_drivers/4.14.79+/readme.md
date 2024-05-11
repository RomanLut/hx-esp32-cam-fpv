Building image for Raspberry Pi Zero W with monitor mode support on internal wireless card(broadcom 43430)

For Linux kernel 4.19.* only!

Whole process will take ~1h.

Installation
-
Download distribution of Raspbian Stretch with 4.14.79+ kernel:

https://downloads.raspberrypi.org/raspbian/images/raspbian-2018-11-15/

Skip updtee step - we have to edit repository first.

Update:

Change your sources.list (https://forums.raspberrypi.com/viewtopic.php?t=356182) to http://legacy.raspbian.org/raspbian/:

```sudo nano /etc/apt/sources.list```

Edit, Ctrl+O, Enter, Ctrl+X.

Update:

```sudo apt-get update```
```sudo apt-get upgrade```

Check kernel version:

```uname -r```

It should be 4.19.66+. **It will not work for kernel versions other then 14.19! **

Install re4son kernel(https://re4son-kernel.com/re4son-pi-kernel/):

```wget -O re4son-kernel_current.tar.xz https://re4son-kernel.com/download/re4son-kernel-current/```

```tar -xJf re4son-kernel_current.tar.xz```

```cd re4son-kernel_4*```

```sudo ./install.sh```

During install process, answer Yes to install kernel headers.

Checking
-
```sudo install airckrack-ng```

```sudo install wireshark tcpdump```

```sudo airmon-ng start wlan0```

(ignore error)

```iwconfig```

=> ```wlan0mon``` should appear, started in monitor mode.

```sudo wireshark```

=> use wireshark to capture packets on ```wlan0mon```

or ```tcpdump -i wlan0mon```

If no packets are received, uncomment other folder in script ad run again.

Building for other kernel versions
-

The easiest way to get monitoring mode support is to install prebuilt re4son kerkel as describeda above. But it is also possible to build kernel yourself.

Check supported cores versions in https://github.com/seemoo-lab/nexmon/tree/master/patches/driver

For full building instructions check  https://github.com/seemoo-lab/nexmon, this is what install.sh above is doing.
