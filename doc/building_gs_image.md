building image

Download distribution of Rapberri Pi OS (Buster 32bit) with 5.10.17-v7+ kernel:
https://downloads.raspberrypi.org/raspios_lite_armhf/images/raspios_lite_armhf-2021-05-28/

Write to SD card using Raspberry PI Imager:
https://www.raspberrypi.com/software/

pi/1234

sudo apt-get update
sudo apt-get upgrade
sudo apt-get autoremove
sudo apt-get autoclean
sudo reboot

sudo raspi-config
Display Options->Resolution->1280x720x60Hz
Interface options->Serial Port-> Shell: No, Hardware enable: Yes
Performance options->GPU Memory: 128



Remove black border:
sudo nano /boot/config.txt
uncomment disable_overscan=1


sudo apt install --no-install-recommends -y libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev libts-dev libfreetype6-dev build-essential autoconf automake libtool libasound2-dev libudev-dev libdbus-1-dev libxext-dev dkms git aircrack-ng 

wget https://www.libsdl.org/release/SDL2-2.0.18.tar.gz
tar zxf SDL2-2.0.18.tar.gz
rm SDL2-2.0.18.tar.gz

cd SDL2-2.0.18
./autogen.sh
./configure --disable-video-rpi --enable-video-kmsdrm --enable-video-x11 --disable-video-opengl
make -j4
sudo make install

Install rtl8812au driver:
cd /home/pi/
git clone https://github.com/svpcom/rtl8812au/
cd rtl8812au
sudo ./dkms-install.sh



cd /home/pi/
git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv
cd esp32-cam-fpv
cd gs
make -j4


sudo nano /etc/rc.local
add a line before before exit 0:
sudo /home/pi/esp32-cam-fpv/gs/launch.sh










