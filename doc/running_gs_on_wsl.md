# Running Ground Station software on WSL

This instruction describes steps for running Ground Station on Windows through WSL2 Ubuntu.

This setup is more complicated than native Linux, because external USB Wifi adapter must be passed from Windows into WSL, and some adapters require an out-of-tree Linux driver.

Ground Station software still requires a Wi-Fi adapter that supports monitor mode and packet injection. The procedure below was tested with Realtek `RTL8812AU`.

## 1. Install WSL2 Ubuntu

Run in Windows PowerShell as Administrator:

```powershell
wsl.exe --install Ubuntu --no-launch --web-download
```

Reboot Windows if requested, then verify:

```powershell
wsl.exe -l -v
```

You should see `Ubuntu` with version `2`.

## 2. Install usbipd-win on Windows

`usbipd-win` is required to attach USB devices to WSL.

```powershell
winget install --id dorssel.usbipd-win -e
```

Verify:

```powershell
& 'C:\Program Files\usbipd-win\usbipd.exe' list
```

## 3. Install build dependencies in Ubuntu

Open Ubuntu once, then install dependencies:

```bash
sudo apt update
sudo apt install --no-install-recommends -y \
  build-essential autoconf automake libtool git pkg-config \
  libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev \
  libturbojpeg0-dev libts-dev libfreetype-dev \
  libasound2-dev libudev-dev libdbus-1-dev libxext-dev \
  libsdl2-dev aircrack-ng dkms
```

## 4. Build Ground Station

If repository is already cloned on Windows drive `D:`, build directly from WSL:

```bash
cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs
make -j4
```

Built binary will be:

```text
/mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs/gs
```

## 5. Prepare Wifi adapter in WSL

### 5.1. Attach USB adapter

Find your adapter:

```powershell
& 'C:\Program Files\usbipd-win\usbipd.exe' list
```

Share it:

```powershell
& 'C:\Program Files\usbipd-win\usbipd.exe' bind --busid <BUSID>
```

Attach it to WSL:

```powershell
& 'C:\Program Files\usbipd-win\usbipd.exe' attach --wsl Ubuntu --busid <BUSID>
```

If `usbipd` says that selected distribution is not running, start Ubuntu first and keep it alive:

```powershell
Start-Process wsl.exe -ArgumentList '-d Ubuntu -u root -- bash -lc "sleep 300"'
```

Then run `attach` again.

### 5.2. Check interface name

Inside Ubuntu:

```bash
iw dev
ip -br link
```

Note interface name, for example:

```text
wlx200db0c4aa3e
```

### 5.3. Driver note for RTL8812AU

Some `RTL8812AU` adapters do not work with stock WSL kernel and require:

* custom WSL kernel
* matching kernel modules installed into WSL
* out-of-tree `rtl8812au` driver built for that exact kernel

If your adapter does not appear in `iw dev`, WSL kernel driver support is incomplete. In that case:

* either build/install `rtl8812au` for your WSL kernel
* or use native Linux instead of WSL

## 6. Launch Ground Station

Manual launch from Ubuntu:

```bash
cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs
./gs -rx <INTERFACE> -tx <INTERFACE> -fullscreen 0 -sm 1
```

If the adapter is already in monitor mode, same command can be used directly.

Use:

```bash
./gs -help
```

to see available command line parameters.

## 7. Helper batch script

This repository also contains:

[`gs/launch_gs_wsl.bat`](../gs/launch_gs_wsl.bat)

It performs:

* start Ubuntu in WSL
* attach USB adapter via `usbipd`
* auto-detect `RTL8812AU` USB bus id
* auto-detect WSL Wifi interface name
* build `gs`
* launch `gs`

Current script still assumes:

* WSL distro: `Ubuntu`
* adapter family: `RTL8812AU` / USB ID `0BDA:8812`

Edit the script if your setup is different.
