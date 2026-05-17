---
name: gs-rpi-sync-deploy
description: Sync changed GS/runtime files to Raspberry Pi over SSH and optionally build remotely.
---

# GS Raspberry Pi Sync Deploy

Use this skill when updating a Raspberry Pi GS runtime at `pi@192.168.3.147` and prefer syncing changes only.

## Default Behavior

- Prefer changed-file sync via `rsync`; do not do full archive copy.
- Do not build remotely unless explicitly requested.
- Always normalize remote `*.sh`/`*.py` scripts to LF after sync, then restore executable flags.

## Required Command (Changed Files Only)

Run from repo root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\sync_changed_gs_target.ps1 `
  -Target rpi4
```

This command already:

- syncs only selected trees via `rsync` (`gs`, `components_gs`, `components/common`, `assets_gs`, `scripts`, and OpenCV trees),
- preserves the project excludes for `gs/build`, runtime artifacts, and OpenCV prebuilt/build outputs,
- converts remote script line endings to LF under `scripts/` and `gs/` (excluding `gs/build`),
- restores executable bits on remote scripts.

## Optional Remote Build

Only when asked:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\sync_changed_gs_target.ps1 `
  -Target rpi4 `
  -Build
```

By default, `-Build` compiles GS only and skips OpenCVWrapper rebuild.

Rebuild OpenCVWrapper only when explicitly necessary:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\sync_changed_gs_target.ps1 `
  -Target rpi4 `
  -Build `
  -BuildOpenCVWrapper
```

Rule: OpenCVWrapper rebuild runs only if `rsync` actually transferred changes under `OpenCV/OpenCVWrapper` in the current sync.  
If no wrapper files changed, rebuild is skipped automatically even when `-BuildOpenCVWrapper` is passed.

## Quick Remote Launch Check

```powershell
& "C:\Program Files\putty\plink.exe" -ssh -batch -no-antispoof -pw 1234 pi@192.168.3.147 "cd /home/pi/esp32-cam-fpv/gs && bash launch.sh"
```

## Notes

- Keep this flow as the default for Raspberry Pi unless the user explicitly asks for full resync/reimage.
- Do not rebuild OpenCVWrapper unless explicitly requested and `rsync` synced wrapper changes.
- If `plink` or WSL `sshpass/rsync` are missing, report the missing dependency and stop before partial deploy.
