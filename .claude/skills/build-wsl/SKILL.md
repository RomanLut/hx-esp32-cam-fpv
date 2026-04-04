---
name: build-wsl
description: Build the Linux gs target via WSL Ubuntu
allowed-tools: Bash
---

Build the Linux `gs` target using this exact command:

```
wsl.exe -d Ubuntu -e bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs && make -j2"
```

Rules:
- Always use `-d Ubuntu` explicitly — never plain `wsl` without it, because the default distro may be `docker-desktop-data` which breaks path mounts
- Use `-lc` (login shell) so that the Ubuntu environment and PATH are fully initialized
- Run the command, show the full output, and report whether the build succeeded or failed
- On failure, show the compiler errors clearly
