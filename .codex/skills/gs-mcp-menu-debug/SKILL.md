---
name: gs-mcp-menu-debug
description: Use when working on esp32-cam-fpv GS debugging over MCP, especially APFPV, Radxa runs, and OSD menu automation. Covers the fixed OSD navigation semantics, MCP tool usage, and the required verification flow before claiming menu or search behavior.
---

# GS MCP Menu Debug

Use this skill for `esp32-cam-fpv` GS debugging when the task involves:
- MCP-driven OSD navigation
- APFPV search/connect debugging
- Radxa launch, MCP reachability, and live GS checks

## OSD Navigation Rules

These rules are fixed. Do not guess alternative controls.

- `Up` / `Down`: move between menu items
- `Enter`: select the current item
- `Left`: go back to the previous menu

Do not assume `Right` activates items unless the user explicitly says the controls changed.

## MCP Tools

The embedded GS MCP server exposes these tools:

- `gs_get_snapshot`
- `gs_get_menu_buffer`
- `gs_press_key`
- `gs_press_keys`

Use `gs_get_snapshot` and `gs_get_menu_buffer` frequently while navigating. Do not navigate blind for more than one step.

## Required Menu Verification Flow

Before claiming anything about menu behavior:

1. Read `gs_get_menu_buffer`.
2. Check `visible`.
3. If `visible` is `false`, treat `lines` and `title` as possibly stale captured output.
4. Open the menu first, then re-read `gs_get_menu_buffer` until `visible=true`.
5. Only then continue navigation.

When navigating:

1. Read the current `title`, `selected_item`, and `lines`.
2. Send the minimum key presses needed.
3. Re-read the menu buffer.
4. Confirm the selection moved as expected before the next step.

## Main Menu Facts

- `Search & Connect...` is the first top-level item in the main OSD menu.
- `Search...` inside the APFPV connect menu is a normal selectable item and must be triggered through menu navigation only.
- `Connect to:` rows appear only after APFPV search populates discovered cameras.

## Radxa Workflow

When testing on Radxa:

1. Check whether `./gs` is already alive and whether MCP is already reachable.
2. If code/assets changed, sync/build before restarting.
3. Launch `gs` only from `/home/radxa/esp32-cam-fpv/gs`.
4. Verify both `./launch.sh` and `./gs` are visible in `ps` before claiming GS is running.
5. Verify the MCP port is reachable before attempting menu automation.
6. Check `/tmp/gs-launch.log` if MCP or APFPV state looks wrong.
7. For persistent remote launch from Windows, prefer a dedicated `tmux` session over `nohup`; on this setup the `tmux` path reliably preserves the same working-directory/TTY behavior as a manual SSH launch.

Useful facts:

- Radxa target: `radxa@192.168.3.148`
- Password: `radxa`
- Remote project path: `/home/radxa/esp32-cam-fpv`
- Remote GS path: `/home/radxa/esp32-cam-fpv/gs`
- Launch log: `/tmp/gs-launch.log`
- Embedded MCP port: `17654`

Radxa commands:

- Sync/build all GS dependencies:
  `powershell -ExecutionPolicy Bypass -File scripts\sync_build_gs_radxa.ps1`
- For small iterative fixes, prefer changed-file sync:
  `powershell -ExecutionPolicy Bypass -File scripts\sync_changed_gs_target.ps1 -Target radxa -Build`
- Remote build:
  `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148 "cd /home/radxa/esp32-cam-fpv/gs && make -j4"`
- Normal launch:
  `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148 "cd /home/radxa/esp32-cam-fpv/gs && ./launch.sh"`
- Preferred persistent remote launch (run from `gs/` so `tmux` starts `./launch.sh` with the right cwd):
  `"C:\Program Files\putty\plink.exe" -ssh -t -pw radxa radxa@192.168.3.148 "cd /home/radxa/esp32-cam-fpv/gs && chmod +x ./launch.sh && (tmux kill-session -t gslaunch 2>/dev/null || true) && tmux new-session -d -s gslaunch ./launch.sh && tmux list-sessions"`
- `tmux` launch verification:
  `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148 "tmux list-sessions 2>/dev/null || true; echo ---; pgrep -af './launch.sh|./gs'; echo ---; tmux capture-pane -pt gslaunch 2>/dev/null | tail -n 80"`
- Background launch verification:
  `"C:\Program Files\putty\plink.exe" -ssh -pw radxa radxa@192.168.3.148 "ps -eo pid,user,cmd | grep -E './launch.sh|./gs' | grep -v grep || true; echo ---; tail -n 80 /tmp/gs-launch.log || true"`

Radxa constraints:

- Windows `tar`/SCP and `rsync --no-perms` drop the execute bit on shell scripts. The Radxa sync PowerShell scripts restore `+x` and CRLF normalization for `scripts/**` and `gs/**` (`*.sh` / `*.py`, excluding `gs/build`). If `./launch.sh` fails with Permission denied, re-run sync or `chmod +x /home/radxa/esp32-cam-fpv/gs/launch.sh` on the board.
- Never sync only `gs\*` manually; Linux `gs` also depends on `components_gs`, `components/common`, and `assets_gs`.
- Sync and remote build are strictly sequential steps on this setup. Never start a Radxa `make` while `rsync` or any other source-sync command is still running.
- When syncing the `gs` tree to Radxa, exclude local build/runtime artifacts such as `gs/build/`, the host-built `gs` binary, and device-local `gs.ini` / `imgui.ini`; pushing those files can break the remote ARM launch or overwrite the board's active runtime mode.
- Restart Radxa `gs` when needed to run a newly built binary or newly synced code/assets.
- Otherwise, do not relaunch Radxa `gs` while an existing `./gs` process is already running unless the user explicitly asks to restart it.
- Do not launch Radxa GS from `~`, `/home/radxa`, or via ad-hoc `nohup ./gs` unless doing targeted low-level debugging.
- Do not treat detached `nohup ./launch.sh` over `plink` as authoritative on this setup; it was shown to be less reliable than `tmux` for preserving the same runtime behavior as a manual launch.

## APFPV Debugging Rules

- Do not claim APFPV search is broken until you confirm the menu path was actually executed.
- Distinguish these cases:
  - menu navigation failed
  - APFPV search ran but found zero cameras
  - cameras were found but `Connect to:` rows were not shown
  - camera connect happened but stream/session did not come up

Use `gs_get_snapshot` to separate them:

- `apfpv.discovered_cameras`
- `apfpv.active_camera_id`
- `session.connected_air_device_id`
- `session.got_config_packet`
- `link_state`

## Reporting Discipline

When reporting results, state:

1. Whether the menu was actually open (`visible=true`)
2. Which menu title was active
3. Which exact keys were sent
4. What changed in the next buffer/snapshot

Do not infer successful navigation from stale menu lines.
