to build project: make -j4

code formatting:
brackets {} are always placed on the next line

building and launching on radxa:
- ssh target: `radxa@192.168.3.148`
- password: `radxa`
- remote project path: `/home/radxa/esp32-cam-fpv`
- remote gs path: `/home/radxa/esp32-cam-fpv/gs`
- use PuTTY tools from Windows for non-interactive remote work:
  - ssh command: `\"C:\\Program Files\\putty\\plink.exe\" -ssh -pw radxa radxa@192.168.3.148`
  - copy command: `\"C:\\Program Files\\putty\\pscp.exe\" -scp -pw radxa`
- sync local gs sources to the radxa before building:
  - `\"C:\\Program Files\\putty\\pscp.exe\" -scp -pw radxa -r gs\\* radxa@192.168.3.148:/home/radxa/esp32-cam-fpv/gs/`
- build on the radxa:
  - `\"C:\\Program Files\\putty\\plink.exe\" -ssh -pw radxa radxa@192.168.3.148 \"cd /home/radxa/esp32-cam-fpv/gs && make -j4\"`
- to launch on the radxa:
  - `\"C:\\Program Files\\putty\\plink.exe\" -ssh -pw radxa radxa@192.168.3.148 \"cd /home/radxa/esp32-cam-fpv/gs && ./launch.sh\"`
