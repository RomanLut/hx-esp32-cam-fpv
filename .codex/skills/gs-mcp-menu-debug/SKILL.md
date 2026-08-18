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

### Quest MCP access

- The Quest GS APK embeds and starts the same MCP server on TCP port `17654`.
- Do not report Quest MCP as unavailable merely because `gs_get_snapshot`, `gs_get_menu_buffer`, or the key tools are absent from the current Codex native tool registry.
- First verify the headset listener with `adb shell ss -ltn` and check Quest logs for `GS MCP server listening`.
- For a USB-attached Quest, create the host tunnel with `adb forward tcp:17654 tcp:17654`, then call the server through `gs/scripts/gs_mcp_client.py` at `127.0.0.1:17654`.
- When launching or refocusing Quest GS through ADB, use its Oculus VR intent: `adb shell am start -a android.intent.action.MAIN -c com.oculus.intent.category.VR -n com.esp32camfpv.questgs/.MainActivity`. A bare component launch can leave the app rendered and Android-resumed while OpenXR remains only `VISIBLE` (`state=4`), so physical controller actions are not polled and MCP input remains disabled.
- In Quest app code, recover VR focus only from a callback proving that the blocking system flow completed, such as Wi-Fi `NetworkCallback.onAvailable`/`onUnavailable` or a permission result. Do not relaunch the VR activity from generic `onResume`, `onWindowFocusChanged`, or startup callbacks: Horizon can invoke them while its approval overlay is still waiting, and an eager relaunch hides the choice before the user can press it.
- After a completion-triggered refocus, verify OpenXR reaches `FOCUSED` and real data/controller input resumes. An Android-resumed activity or visible rendering is not sufficient.
- Native Codex tool exposure and embedded-server reachability are separate: native exposure requires MCP configuration and a new Codex session, while the direct client works through the ADB tunnel in the current session.
- Only report Quest MCP unavailable after both the embedded listener and the direct forwarded client have been checked.
- Before every MCP key injection on Quest, verify that Quest GS owns user input focus and that no Android permission dialog, Wi-Fi network-request dialog, Horizon panel, or other system overlay is focused or visibly blocking it.
- Treat `OpenXR: session state=5` (`XR_SESSION_STATE_FOCUSED`) as the authoritative application-focus signal. If the latest state is not focused or cannot be established, do not inject a key.
- A captured menu with `visible=true` only proves that the renderer processed earlier input; it does not prove the user can currently see or operate that menu.
- Never use MCP to perform a menu action that the user could not perform with the controller in the current focus state.
- Quest USB permission requests must be single-flight per device. A periodic controller sync must not call `UsbManager.requestPermission()` again while a request is pending, and a denial must suppress further prompts until the USB topology or explicit device selection changes.
- Serialize USB permission requests across every Quest controller, not only inside each controller. Horizon can keep only one of simultaneous RAW/scan/telemetry requests visible while another controller remains stuck in a pending state. Release the app-wide permission owner on result, observed grant, detach, controller stop, and transport or device-selection change. Treat `UsbManager.hasPermission()==true` as authoritative because Horizon can grant access without delivering the dynamic permission-result broadcast.
- Treat the USB permission-result broadcast as the completion signal for VR focus recovery. Do not recover focus merely because the activity resumed while the Horizon permission overlay is still open.
- A fast Quest USB hub replug may reuse the same `/dev/bus/usb/...` device names. Track a detach generation and rebuild native USB objects after every observed detach even when names and adapter counts match; also keep periodic reconciliation alive after detach/open exceptions so devices can recover without restarting GS.

### Quest unfocused-state workflow

When Quest GS is not in `XR_SESSION_STATE_FOCUSED`, do not stop at reporting that MCP input is unavailable. Determine what currently owns user focus and continue debugging through that visible UI when appropriate:

1. Use `adb shell dumpsys activity activities` and `adb shell dumpsys window windows` to identify the focused activity/window and its display and bounds.
2. Capture the current headset display with `adb shell screencap -p`, pull it to a temporary host path, and visually inspect it before acting. `uiautomator dump` may expose only the VR shell hierarchy on Quest, so an empty or irrelevant hierarchy is not proof that no dialog is visible.
3. Read the exact visible message, choices, and target. Do not guess that a dialog is an approval prompt; it may instead be an error, retry prompt, permission request, or another state.
4. If the user has explicitly authorized the visible system action, interact with the focused Android/Horizon dialog using a controller-equivalent key or a tap on a verified visible control. This is system-UI input, not an MCP menu key.
5. Send only one action at a time. Then re-check the focused window, capture another screenshot when the result is visual, and inspect the relevant service state or logs before taking another action.
6. Never redirect input to Quest GS while another window owns focus. Resume MCP menu input only after the latest OpenXR state is `XR_SESSION_STATE_FOCUSED` and the MCP snapshot reports `synthetic_input_enabled=true` when that field is available.

For Wi-Fi network-request dialogs, also verify the outcome in `adb shell dumpsys wifi`. Distinguish user approval (`mUserApprovedAccessPointMap`) from an actual network connection (`mWifiInfo`, the app's `NetworkCallback`, and the GS snapshot); approval alone does not prove that the headset connected or that video packets are arriving.

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

- Never use a remembered or hard-coded Radxa IP address. Resolve it from `gs/sync_changed_and_run_radxa.bat` before each connection, honoring an existing `RADXA_IP_ADDRESS` environment override first.
- PowerShell address resolution:
  `$radxaIp = if ($env:RADXA_IP_ADDRESS) { $env:RADXA_IP_ADDRESS } else { (Select-String -Path "gs/sync_changed_and_run_radxa.bat" -Pattern 'RADXA_IP_ADDRESS=([^\"]+)').Matches[0].Groups[1].Value }`
- Radxa target: `radxa@$radxaIp`
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
  `& "C:\Program Files\putty\plink.exe" -ssh -pw radxa "radxa@$radxaIp" "cd /home/radxa/esp32-cam-fpv/gs && make -j4"`
- Normal launch:
  `& "C:\Program Files\putty\plink.exe" -ssh -pw radxa "radxa@$radxaIp" "cd /home/radxa/esp32-cam-fpv/gs && ./launch.sh"`
- Preferred persistent remote launch (run from `gs/` so `tmux` starts `./launch.sh` with the right cwd):
  `& "C:\Program Files\putty\plink.exe" -ssh -t -pw radxa "radxa@$radxaIp" "cd /home/radxa/esp32-cam-fpv/gs && chmod +x ./launch.sh && (tmux kill-session -t gslaunch 2>/dev/null || true) && tmux new-session -d -s gslaunch ./launch.sh && tmux list-sessions"`
- `tmux` launch verification:
  `& "C:\Program Files\putty\plink.exe" -ssh -pw radxa "radxa@$radxaIp" "tmux list-sessions 2>/dev/null || true; echo ---; pgrep -af './launch.sh|./gs'; echo ---; tmux capture-pane -pt gslaunch 2>/dev/null | tail -n 80"`
- Background launch verification:
  `& "C:\Program Files\putty\plink.exe" -ssh -pw radxa "radxa@$radxaIp" "ps -eo pid,user,cmd | grep -E './launch.sh|./gs' | grep -v grep || true; echo ---; tail -n 80 /tmp/gs-launch.log || true"`

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
