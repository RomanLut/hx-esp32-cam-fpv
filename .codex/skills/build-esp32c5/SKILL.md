---
name: build-esp32c5
description: Build, rebuild, clean, flash, or package OTA and merged artifacts for the repository's air_firmware_esp32c5 target on Windows. Use for ESP32-C5 PlatformIO compilation, USB Serial/JTAG upload, firmware_ota creation, factory/merged image creation, or troubleshooting Windows command-line-too-long failures in this project.
---

# Build ESP32-C5 Firmware

Use `scripts/build-esp32c5.ps1` for every operation. It establishes the short paths required by ESP-IDF on Windows, selects the project's ESP-IDF 5.5.5 package under a stable short directory, and refuses to overlap another firmware build.

## Workflow

Run commands from the repository root:

```powershell
& '.\.codex\skills\build-esp32c5\scripts\build-esp32c5.ps1' -Action build
```

Never run this target concurrently with any ESP32 firmware build or clean. Use only `C:\Users\roman\.platformio\penv\Scripts\pio.exe`; never use global `pio`, `platformio`, or `python -m platformio`.

Do not clean routinely. Use `-Action clean` only when CMake regeneration is proven necessary, such as a build-directory/source-path mismatch or missing generated CMake metadata. After a timed-out PlatformIO call, inspect PlatformIO, Python, CMake, Ninja, and RISC-V compiler processes and wait until the original process exits before taking another action.

## Flash

First place the C5 in USB download mode. Enumerate only present ports; never choose a historical/absent COM device.

```powershell
& '.\.codex\skills\build-esp32c5\scripts\build-esp32c5.ps1' -Action upload
```

The script auto-selects one present Espressif VID `303A` port. If multiple are present, pass `-Port COMx`. If none is present, do not infer that the connected board is missing: the running application intentionally reassigns the USB pins to UART. Require ROM download mode, usually by holding BOOT, tapping RESET, then releasing BOOT.

Flash with direct esptool using `--before no-reset`; PlatformIO's default upload reasserts its own reset sequence after the user has already established download mode. Force UTF-8 Python output because esptool 5.3's Unicode progress bar raises `UnicodeEncodeError` under this machine's CP1251 console before completing the write.

After upload, the helper checks USB state. The native USB COM port disappearing normally means the application booted because the firmware reassigns those pins to UART. If the COM port remains, the helper connects without a stub and requests the C5 watchdog reset:

```powershell
& 'C:\Users\roman\.platformio\penv\Scripts\python.exe' -m esptool --chip esp32c5 --port COMx --no-stub --before no-reset --after watchdog-reset chip-id
```

Treat disappearance after the watchdog reset as expected application startup. Verify `boot:0x19 (SPI_FAST_FLASH_BOOT)` and ESP-IDF application logs when serial remains available. Do not use the watchdog reset through the flasher stub.

## Package OTA artifacts

After a successful build, run:

```powershell
& '.\.codex\skills\build-esp32c5\scripts\build-esp32c5.ps1' -Action package -OpenExplorer
```

This calls the repository's canonical `.github/scripts/package_firmware.py`, derives the version from `FW_VERSION` and `PACKET_VERSION`, and creates these files under `firmware_artifacts/air_firmware_esp32c5`:

- `air_firmware_esp32c5_ota.bin`: application-only OTA image.
- `air_firmware_esp32c5_merged.bin`: complete image for offset `0x0`.
- `air_firmware_esp32c5_manifest.json`: ESP Web Tools manifest.
- `air_firmware_esp32c5.zip`: packaged release artifacts.

Report the build/upload outcome and the OTA path and SHA-256. When requested, open Explorer with the OTA file selected.

## Root cause preserved by this skill

ESP-IDF's linker generator repeats the framework directory many times. A hashed PlatformIO package path produced an 8,654-character batch command, exceeding Windows `cmd.exe`'s 8,191-character limit. Retrying or cleaning does not fix that defect. The helper maps the repository and PlatformIO core to `Q:` and `P:` and gives the exact ESP-IDF package the stable `framework-espidf` directory name before invoking PlatformIO.
