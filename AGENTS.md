file reference formatting:
- always use markdown links for file references
- always use absolute filesystem paths
- always start the path with `/`
- valid example: `[name](/d:/path/to/file.ts)`
- valid line example: `[name](/d:/path/to/file.ts#L123)`
- do not use `file://`
- do not use plain inline code for clickable file references

root-cause fixes:
- do not implement recovery, restart, replug, retry, timeout, or state-reset workarounds as substitutes for a requested root-cause fix
- trace the failure to the owning layer, fix the causal defect there, and verify the complete user-visible workflow
- if the root cause is not yet proven, continue diagnosis or report the remaining evidence gap instead of presenting an ad hoc mitigation as a fix

code style and comments:
- brackets `{}` are always placed on the next line
- every out-of-class function or method definition, and every class or struct definition, should start with:
  `//===================================================================================`
  `//===================================================================================`
- after these two lines, add a short English description comment explaining what it does
- when adding these separator lines, check the existing nearby comments first and do not duplicate them; each definition should start with exactly two `//===================================================================================` lines, not four or more
- do not add these separator lines or description comments to method declarations inside a class or struct body
- if you change an implementation, update any stale comments that describe that implementation, especially the leading comment above the definition and the leading comment above the class or struct declaration
- if testing or debugging reveals a non-obvious behavior, constraint, race, ordering requirement, or hardware/platform quirk, add a short English comment near the relevant implementation that explains the behavior and why the code is written that way
- do not leave important debug findings only in chat, commit messages, or temporary notes; preserve them in the code where a future engineer would otherwise repeat the same mistake

first-party GS logging rule:
- in first-party GS code, including `components_gs`, `gs/src`, and `android_gs/app/src/main/cpp`, always use shared `LOGD` / `LOGI` / `LOGW` / `LOGE` macros from `components_gs/shared/Log.h` for logging
- do not use `__android_log_print`, `printf`, `fprintf`, or ad-hoc logging macros in first-party GS code
- if platform-specific logging behavior is needed, implement it inside `components_gs/shared/Log.h`, not at call sites
- treat vendored third-party code under `components_gs/imgui`, `components_gs/fmt`, `android_gs/app/src/main/cpp/third_party`, and similar imported libraries as exceptions unless explicitly asked to modify them

Radxa deployment rule:
- when updating Radxa sync or install flows, deploy the top-level scripts directory with the GS runtime tree
- after syncing to Radxa, normalize remote shell and Python scripts to LF line endings and restore executable flags because Windows and rsync options may not preserve them; this must include [`gs/launch.sh`](/d:/Github/esp32-cam-fpv/esp32-cam-fpv/gs/launch.sh) and any other `*.sh` / `*.py` under the synced `gs/` tree (excluding `gs/build`), not only files under `scripts/`

Linux script copy rule:
- whenever a script is copied from Windows workspace to any Linux target (Raspberry Pi, Radxa, WSL, or other), convert it to LF on target before execution (for example `sed -i 's/\r$//' <script>`), then restore executable flags (`chmod +x`) as needed

ESP32 PlatformIO build rule:
- never run ESP32 PlatformIO builds or clean commands concurrently; all firmware targets share PlatformIO packages and tools, and concurrent runs cause package-manager collisions and locked build files
- build `air_firmware_esp32cam`, `air_firmware_esp32s3sense`, and `air_firmware_esp32c5` strictly one at a time, including when using orchestration tools; do not use `Promise.all`, parallel jobs, or overlapping shells for these targets
- use `C:\Users\roman\.platformio\penv\Scripts\pio.exe` instead of the global `pio` command or `python -m platformio`
- do not clean a target unless regeneration is actually required; a clean ESP-IDF build is much slower than an incremental build
- allow at least 15 minutes for a clean ESP32 build command so the tool wrapper does not time out while the compiler is still working
- if any PlatformIO command times out, assume its child processes may still be running; inspect PlatformIO, Python, CMake, Ninja, and Xtensa/RISC-V compiler processes and wait for the original build to finish before starting another build or clean command
- never start a retry while any prior PlatformIO or compiler process for this workspace is alive, because the overlapping process can lock archives, linker scripts, and generated files
- after a timed-out wrapper finishes in the background, verify its result from the completed process/artifacts and, if needed, run only one incremental confirmation after all prior processes have exited

ESP32-C5 USB reset rule:
- after flashing an ESP32-C5 over USB Serial/JTAG, a normal esptool hard reset or manual RTS pulse can leave the chip in the ROM loader with `boot:0xf (DOWNLOAD(UART0/USB))`; do not conclude that the application or flash is broken from this state
- the C5 application reassigns the native USB pins to UART, so the COM port disappearing immediately after reset normally means the application booted successfully; use the watchdog-reset recovery only when the COM port remains present in ROM download mode
- to start the flashed application without using the external reset/boot lines, connect to the existing ROM loader without a stub and request the C5 watchdog reset:
  `C:\Users\roman\.platformio\penv\Scripts\python.exe -m esptool --chip esp32c5 --port <PORT> --no-stub --before no-reset --after watchdog-reset chip-id`
- `--no-stub` is required because esptool's watchdog reset does not work through the flasher stub; `--before no-reset` is required so esptool does not first reassert the external download/reset sequence
- verify success from serial output showing `boot:0x19 (SPI_FAST_FLASH_BOOT)` followed by the ESP-IDF bootloader and application logs

