# Building Ground Station app to run on a Fedora Linux desktop

The Ground Station application can run on a desktop Linux (no Raspberry Pi required). This manual explains how to install and run the GS app on Fedora Linux.

We will assume you are using a dedicated RTL8812AU WiFi adapter on interface `wlp0s20f0u1`.

* Install [rtl8812au driver](https://github.com/svpcom/rtl8812au/).
* Request NetworkManager to do not manage your adapter:
```
nmcli dev set wlp0s20f0u1 managed no
```
* Install dependencies:
```
sudo yum install SDL2-devel turbojpeg-devel freetype-devel
```
* Download the GS code:
```
git clone -b release --recursive https://github.com/RomanLut/esp32-cam-fpv
```
* Build the app:
```
cd esp32-cam-fpv/gs
make -j4
```
* Launch GS:
```
sudo ./gs -rx wlp0s20f0u1 -tx wlp0s20f0u1
```
