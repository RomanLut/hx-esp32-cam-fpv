---
name: build-wsl
description: Build the Linux gs target via WSL Ubuntu.
---

# Build Linux GS In WSL

Build the Linux `gs` target using this exact command:

```powershell
wsl.exe -d Ubuntu -e bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs && make -j2"
```

Rules:

- Always use `-d Ubuntu` explicitly; never use plain `wsl` without it because the default distro may be `docker-desktop-data` and break path mounts.
- Use `-lc` so the Ubuntu environment and `PATH` are initialized.
- Run the command, report whether the build succeeded or failed, and surface compiler errors clearly on failure.
