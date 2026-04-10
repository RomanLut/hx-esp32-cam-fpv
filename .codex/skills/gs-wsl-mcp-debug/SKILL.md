---
name: gs-wsl-mcp-debug
description: Use when debugging Linux GS through WSL with the embedded MCP server. Covers the stable visible-window launch, readiness checks, and Windows localhost access through the configured MCP port proxy.
---

# GS WSL MCP Debug

Use this skill when the task is to debug Linux `gs` over WSL through the embedded MCP server.

## Use The Project Scripts

Do not improvise `PowerShell -> wsl.exe -> bash -> python` heredoc chains for MCP work. They are fragile and were already shown to fail on quoting and process lifetime.

Use these project scripts instead:

- Launch MCP-ready `gs`: [launch_gs_wsl.bat](/d:/Github/esp32-cam-fpv/esp32-cam-fpv/gs/launch_gs_wsl.bat) with `-mcp`
- Stop the MCP debug instance: [launch_gs_wsl.bat](/d:/Github/esp32-cam-fpv/esp32-cam-fpv/gs/launch_gs_wsl.bat) with `-mcp-stop`
- Call the embedded MCP server from inside WSL with [gs_mcp_client.py](/d:/Github/esp32-cam-fpv/esp32-cam-fpv/gs/scripts/gs_mcp_client.py), or from Windows through `127.0.0.1:17654` after `-mcp` reports readiness.

## Build And Foreground Launch

- Always use the `Ubuntu` WSL distro, not the default WSL distro.
- Build Linux `gs` with:
  `wsl.exe -d Ubuntu -e bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs && make -j2"`
- Normal foreground launch:
  `wsl.exe -d Ubuntu -u root -- bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs && exec ./gs -rx <iface> -tx <iface> -fullscreen 0 -sm 1"`
- Short smoke test launch:
  `wsl.exe -d Ubuntu -u root -- bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs && timeout 25s ./gs -rx <iface> -tx <iface> -fullscreen 0 -sm 1"`
- Use `-lc` for WSL build/launch commands so the Ubuntu environment and PATH are initialized.
- Do not use plain `wsl ...`; the default distro may be `docker-desktop-data` and break path mounts/build execution.
- Run the build command, report whether the build succeeded or failed, and surface compiler errors clearly on failure.

## Required Workflow

1. Start the WSL MCP instance with:
   `.\gs\launch_gs_wsl.bat -mcp`
2. Wait for the script to report readiness.
3. Confirm Windows-side MCP access if needed:
   `Test-NetConnection 127.0.0.1 -Port 17654`
4. Run MCP calls from inside WSL, for example:
   `wsl.exe -d Ubuntu -u root -- python3 /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs/scripts/gs_mcp_client.py snapshot`
5. When finished, stop the instance with:
   `.\gs\launch_gs_wsl.bat -mcp-stop`

## Non-Obvious Constraints

- The embedded MCP server listens inside the WSL `gs` process. The `-mcp` launcher configures Windows `127.0.0.1:17654` with `netsh interface portproxy`; do not assume Windows localhost works before `-mcp` reports readiness.
- Querying MCP from inside WSL remains the lowest-level verification path because it bypasses Windows localhost/proxy issues.
- Always stop stale `gs` instances before a new MCP run. Otherwise port `17654` may already be occupied and the new process will fail to bind.
- Do not use short foreground `timeout 25s ./gs ...` launches when you need MCP automation. That mode is fine for quick smoke tests, but it is too short-lived for stable MCP interaction.
- Wait for both process liveness and MCP responsiveness before claiming the server is ready. A running `gs` process alone is not enough.
- In the stable WSL MCP path, `gs` is launched in a normal visible dedicated WSL window. Do not hide the window and do not use a fully detached `nohup` or hidden `Start-Process` launch; detached/headless WSL launches were shown to make `gs` disappear before MCP automation could use it.

## Suggested MCP Calls

- Snapshot:
  `wsl.exe -d Ubuntu -u root -- python3 /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs/scripts/gs_mcp_client.py snapshot`
- Menu buffer:
  `wsl.exe -d Ubuntu -u root -- python3 /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs/scripts/gs_mcp_client.py menu-buffer`
- Inject one key:
  `wsl.exe -d Ubuntu -u root -- python3 /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs/scripts/gs_mcp_client.py press-key Enter`
- Inject multiple keys:
  `wsl.exe -d Ubuntu -u root -- python3 /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv/gs/scripts/gs_mcp_client.py press-keys Down Enter`

## Reporting Discipline

When reporting a WSL MCP result, state:

1. Which launch script was used
2. Which interface was used
3. Whether MCP readiness was confirmed by an actual MCP call
4. Which exact MCP command was run
5. Whether Windows `127.0.0.1:17654` was confirmed with `Test-NetConnection` when Windows-side access is part of the task
