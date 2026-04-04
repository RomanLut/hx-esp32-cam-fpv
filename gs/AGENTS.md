to build project: make -j4

code formatting:
brackets {} are always placed on the next line

building and launching on radxa:
- ssh target: `radxa@192.168.3.148`
- password: `radxa`
- remote project path: `/home/radxa/esp32-cam-fpv`
- remote gs path: `/home/radxa/esp32-cam-fpv/gs`
- use PuTTY tools from Windows for non-interactive remote work:
  - ssh command: `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148`
  - copy command: `"C:\Program Files\putty\pscp.exe" -scp -pw radxa`
- never sync only `gs\*` to the radxa. The Linux `gs` build also depends on `components_gs` and runtime launch also needs `assets_gs`.
- preferred sync/build command:
  - `powershell -ExecutionPolicy Bypass -File scripts\sync_build_gs_radxa.ps1`
  - the helper syncs only the current working-tree content needed by Linux `gs`: `gs`, `components_gs`, `components/common`, and `assets_gs`
- for small iterative fixes, prefer changed-file sync so Radxa keeps its incremental build cache:
  - `powershell -ExecutionPolicy Bypass -File scripts\sync_changed_gs_radxa.ps1 -Build`
- if you must sync manually, copy all required directories before building or launching:
  - `"C:\Program Files\putty\pscp.exe" -scp -pw radxa -r gs\* radxa@192.168.3.148:/home/radxa/esp32-cam-fpv/gs/`
  - `"C:\Program Files\putty\pscp.exe" -scp -pw radxa -r components_gs\* radxa@192.168.3.148:/home/radxa/esp32-cam-fpv/components_gs/`
  - `"C:\Program Files\putty\pscp.exe" -scp -pw radxa -r assets_gs\* radxa@192.168.3.148:/home/radxa/esp32-cam-fpv/assets_gs/`
- build on the radxa:
  - `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148 "cd /home/radxa/esp32-cam-fpv/gs && make -j4"`
- to launch on the radxa:
  - `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148 "cd /home/radxa/esp32-cam-fpv/gs && ./launch.sh"`
