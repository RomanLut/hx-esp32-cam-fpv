#!/bin/bash
sudo apt install raspberrypi-kernel-headers git libgmp3-dev gawk qpdf bison flex make autoconf libtool texinfo

cd /home/pi/
git clone https://github.com/seemoo-lab/nexmon.git

cd nexmon
cd buildtools/isl-0.10
./configure
make
make install
ln -s /usr/local/lib/libisl.so /usr/lib/arm-linux-gnueabihf/libisl.so.10

cd /home/pi/
cd nexmon
cd buildtools/mpfr-3.1.4
autoreconf -f -i
./configure
make
make install
ln -s /usr/local/lib/libmpfr.so /usr/lib/arm-linux-gnueabihf/libmpfr.so.4

cd /home/pi/
cd nexmon
source setup_env.sh
make

# driver 43430 1
cd /home/pi/
cd nexmon
#------------------------- Uncomment right version of the card here ----------------
#cd patches/bcm43430a1/7_45_41_26/nexmon/
cd patches/bcm43430a1/7_45_41_46/nexmon/
#cd patches/bcm43455c0/7_45_154/nexmon/
#cd patches/bcm43455c0/7_45_154/nexmon/
#cd patches/bcm43455c0/7_45_189/nexmon/
#cd patches/bcm43455c0/7_45_206/nexmon/
#cd patches/bcm43455c0/7_45_241/nexmon/
#cd patches/bcm43436b0/9_88_4_65/nexmon/
#cd patches/bcm43455c0/7_45_206/nexmon/
make
make backup-firmware
make install-firmware

cd /home/pi/
cd nexmon
cd utilities/nexutil/
make && make install

sudo iw dev wlan0 set power_save off
sudo cp /home/pi/nexmon/patches/driver/brcmfmac_5.10.y-nexmon/brcmfmac.ko /lib/modules/5.10.103-v7+/kernel/drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko
sudo depmod -a


