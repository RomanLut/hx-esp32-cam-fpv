file reference formatting:
- always use markdown links for file references
- always use absolute filesystem paths
- always start the path with `/`
- valid example: `[name](/d:/path/to/file.ts)`
- valid line example: `[name](/d:/path/to/file.ts#L123)`
- do not use `file://`
- do not use plain inline code for clickable file references

wsl linux build rule:
- always build the linux `gs` target via `wsl.exe -d Ubuntu`, never via the default WSL distro
- exact command:
  `wsl.exe -d Ubuntu -e bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs && make -j2"`
- do not use plain `wsl ...` without `-d Ubuntu`, because the default distro may be `docker-desktop-data` and break path mounts/build execution

code definition comment rule:
- every function, method, class, and struct definition should start with:
  `//===================================================================================`
  `//===================================================================================`
- after these two lines, add a short description comment explaining what it does
