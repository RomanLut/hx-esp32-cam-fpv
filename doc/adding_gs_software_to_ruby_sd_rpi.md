# Adding hx-esp32-cam-fpv GS software to an existing RubyFPV Raspberry Pi SD card

## Automated installation

1. Connect the Raspberry Pi GS to the LAN with a USB-LAN adapter.
2. Boot the **RubyFPV** image.
3. Enable SSH in **System > Network > Enable SSH** in the Ruby interface.
4. Connect over SSH. The default RubyFPV credentials are `pi` / `raspberry`.
5. Update the system time:

   ```bash
   sudo timedatectl set-ntp true
   ```

6. Stop Ruby before building. This releases memory and prevents its radio and
   video processes from competing with the compiler:

   ```bash
   sudo killall ruby_logger ruby_controller ruby_central ruby_i2c \
     ruby_rt_station ruby_rx_telemetry ruby_tx_rc ruby_player_p 2>/dev/null || true
   pgrep -a ruby_ || true
   ```

   The final command should not list any Ruby processes.

7. Download and run the installer:

   ```bash
   wget https://raw.githubusercontent.com/RomanLut/hx-esp32-cam-fpv/refs/heads/release/scripts/install_on_ruby.sh
   chmod +x install_on_ruby.sh
   ./install_on_ruby.sh
   ```

Wait until the script finishes and reboots into **hx-esp32-cam-fpv** GS. Keep
the SSH connection open until the reboot. Installation can take approximately
one hour on a Raspberry Pi Zero 2W.

The installer also installs the matching Raspberry Pi kernel headers, builds
the pinned RubyFPV RTL8812AU v5.2.20 driver with the APFPV fixes, and replaces
both copies of the module:

- `/lib/modules/$(uname -r)/kernel/drivers/net/wireless/88XXau.ko`, loaded by
  the running system;
- `/home/pi/ruby/drivers/88XXau-pi+.ko` on Raspberry Pi 4, or
  `/home/pi/ruby/drivers/88XXau-pi.ko` on the other supported Raspberry Pi
  kernel variants. Ruby uses this packaged copy during a driver reset.

Replacing both files is required. Otherwise, Ruby can restore its original
module and reintroduce the APFPV RSSI/state and cfg80211 problems.

After reboot, verify the module and both copies. Use `sudo` because Ruby's
driver directory is root-only on some images:

```bash
lsmod | grep 88XXau
sudo modinfo -F name /lib/modules/$(uname -r)/kernel/drivers/net/wireless/88XXau.ko
sudo modinfo -F vermagic /lib/modules/$(uname -r)/kernel/drivers/net/wireless/88XXau.ko
sudo sha256sum /home/pi/ruby/drivers/88XXau-pi+.ko \
  /lib/modules/$(uname -r)/kernel/drivers/net/wireless/88XXau.ko
```

On Raspberry Pi 4, the loaded module should be `88XXau_wfb`, and the two
SHA-256 values must be identical. Use `88XXau-pi.ko` in the checksum command on
a platform where that is Ruby's packaged driver.

See also [Installing Fan Control Service](/doc/installing_fan_control_service.md).

## Manual installation

The following is the equivalent manual procedure.

1. Download the latest **RubyFPV** Raspberry Pi image from
   https://rubyfpv.com/downloads.php and write it with Raspberry Pi Imager by
   selecting **Other OS**.
2. Boot the image, enable SSH, connect as `pi`, update the time, and stop the
   Ruby processes as shown in the automated procedure.
3. On legacy Buster images, replace expired Raspbian package sources with the
   Debian archive sources used by `install_on_ruby.sh` before running
   `apt update`.
4. Install the required packages and matching kernel headers:

   ```bash
   sudo apt update
   sudo apt install --no-install-recommends -y \
     libdrm-dev libgbm-dev libgles2-mesa-dev libpcap-dev libturbojpeg0-dev \
     libts-dev libfreetype6-dev build-essential autoconf automake libtool \
     libasound2-dev libudev-dev libdbus-1-dev libxext-dev libsdl2-dev dkms \
     git aircrack-ng cmake raspberrypi-kernel-headers
   ```

5. Build and install SDL 2.32.10. Use one build job on RubyFPV Raspberry Pi
   images to keep memory usage predictable:

   ```bash
   cd ~
   wget https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
   tar zxf SDL2-2.32.10.tar.gz
   cd SDL2-2.32.10
   ./autogen.sh
   ./configure --disable-video-rpi --enable-video-kmsdrm \
     --enable-video-x11 --disable-video-opengl
   make -j1
   sudo make install
   ```

6. Clone the repository:

   ```bash
   cd ~
   git clone -b release --recursive --shallow-submodules \
     https://github.com/RomanLut/esp32-cam-fpv
   cd esp32-cam-fpv
   ```

7. Build and install the patched Ruby RTL8812AU driver:

   ```bash
   MAKE_JOBS=1 bash scripts/install_ruby_rtl8812au_driver.sh
   ```

   The helper verifies the internal module name and kernel vermagic before it
   atomically replaces Ruby's packaged copy and the kernel copy.

8. Build OpenCVWrapper when CMake 3.18 or newer is available, then build GS:

   ```bash
   BUILD_JOBS=1 bash OpenCV/OpenCVWrapper/scripts/build_linux.sh
   cd gs
   make -j1
   ```

   The Buster archive currently supplies CMake 3.16.3. The automated installer
   skips OpenCVWrapper on that image and still builds the GS executable.

9. Edit `/root/.profile`: comment out `./ruby_start` and the associated
   `echo "Launch done."` line, remove any old duplicate boot-selection line,
   and add:

   ```bash
   /home/pi/esp32-cam-fpv/scripts/boot_selection.sh
   ```

10. Reboot:

    ```bash
    sudo reboot
    ```

## Persistent USB-LAN management address

`airmon-ng check kill` stops DHCP/network-manager processes while GS prepares
the Wi-Fi adapters for monitor mode. Therefore, a DHCP address is not a
reliable management address while GS is running. Exiting GS may start
`dhcpcd`, but that does not provide persistent in-GS access on every Ruby image.

For a persistent wired management address, configure the USB-LAN interface
with standard `ifupdown`. Choose an unused address suitable for your LAN. This
example configures `192.168.3.147/24` with gateway `192.168.3.1`:

```bash
sudo tee /etc/network/interfaces.d/esp32camfpv-eth0 >/dev/null <<'EOF'
allow-hotplug eth0
iface eth0 inet static
    address 192.168.3.147
    netmask 255.255.255.0
    gateway 192.168.3.1
EOF

sudo systemctl disable dhcpcd
sudo systemctl enable networking
sudo reboot
```

After reboot, verify the address and route:

```bash
ip -4 -br addr show eth0
ip route
ping -c 3 192.168.3.1
```

If the USB adapter is not named `eth0`, find it with `ip -br link` and use its
actual name in both the filename contents and the verification commands.

## Unscaled 1280x720 HDMI output

RubyFPV images can force overscan margins in `/boot/cmdline.txt`. These margins
scale the GS framebuffer into a smaller output area even when both GS and HDMI
use 1280x720.

Keep the GS HDMI mode at CEA mode 4 (1280x720p60) and disable overscan in
`/boot/config.txt`:

```ini
disable_overscan=1
hdmi_group=1
hdmi_mode=4
```

Remove the complete `video=HDMI-A-1:...` argument, including all
`margin_left`, `margin_right`, `margin_top`, and `margin_bottom` values, from
the single line in `/boot/cmdline.txt`. Do not add a newline to that file.
Reboot, then verify that the framebuffer and HDMI timing both use 1280x720:

```bash
fbset -s
tvservice -s
```


## Updating the ground station

Stop Ruby or GS before rebuilding, then update and rebuild:

```bash
cd ~/esp32-cam-fpv
git pull
MAKE_JOBS=1 bash scripts/install_ruby_rtl8812au_driver.sh
BUILD_JOBS=1 bash OpenCV/OpenCVWrapper/scripts/build_linux.sh
cd gs
make -j1
sudo reboot
```

Rebuild the patched driver whenever the kernel, driver helper, or APFPV patch
changes.

## Notes

- If an image is booted on Raspberry Pi 4 once, it may no longer boot on a
  Raspberry Pi Zero 2W.
- Development notes for RubyFPV images are in
  [VS Code Remote Development](/doc/vs_code_remote_development.md).
