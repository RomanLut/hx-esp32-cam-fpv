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
- every out-of-class function or method definition, and every class or struct definition, should start with:
  `//===================================================================================`
  `//===================================================================================`
- after these two lines, add a short description comment explaining what it does
- do not add these separator lines or description comments to method declarations inside a class or struct body
- all comments must be in English

components_gs logging rule:
- in first-party `components_gs` code, always use shared `LOGD` / `LOGI` / `LOGW` / `LOGE` macros for logging
- do not use `__android_log_print`, `printf`, `fprintf`, or ad-hoc logging macros in first-party `components_gs` code
- if platform-specific logging behavior is needed, implement it inside `components_gs/shared/Log.h`, not at call sites
- treat vendored third-party code under `components_gs/imgui`, `components_gs/fmt`, and similar imported libraries as exceptions unless explicitly asked to modify them
