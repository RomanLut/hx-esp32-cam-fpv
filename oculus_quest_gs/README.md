# Oculus Quest GS

Ground Station port of esp32-cam-fpv for Oculus Quest 2 / 3 / Pro.
Renders the FPV stream and full GS UI into a head-locked OpenXR quad layer.

## Scope

- Standalone Android (Quest) app project
- Kotlin + Compose app shell (no on-screen Compose content; all visuals are the OpenXR layer)
- NDK/CMake native library shared with the legacy `android_gs/` build
- Reusable GS core (`components_gs/`) linked in
- Minimum Android version: 10 (API 29) — Quest 2 baseline
- Compile/target SDK: 34

## Architecture

Two GL contexts in the same EGL share group, each on its own thread:

1. **OpenXR thread** ([openxr_quest_runtime.cpp](app/src/main/cpp/openxr_quest_runtime.cpp))
   - Owns the EGL context (master). Publishes it to the bridge on init.
   - Drives the `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` loop at headset
     refresh rate.
   - Each frame samples the renderer's offscreen color texture into a 1280×720
     `XrCompositionLayerQuad` swapchain (head-locked, VIEW reference space).
   - Reads controllers via the OpenXR action system: A→`ImGuiKey_G`,
     B→`ImGuiKey_R`, index trigger / thumbstick-click→`ImGuiKey_Enter`,
     thumbstick directions→arrow keys. Results are pushed into a small queue
     in the bridge.

2. **Renderer thread** (shared `GsVideoRenderer` from `components_gs/shared/`)
   - Owns a tiny pbuffer EGL surface in the OpenXR thread's share group
     (created in [android_surface_backend.cpp](app/src/main/cpp/android_surface_backend.cpp)).
     The pbuffer is just a placeholder for `eglMakeCurrent`; nothing is ever
     drawn to it.
   - Renders FPV video + ImGui overlays into a 1280×720 offscreen FBO whose
     color texture is shared with the OpenXR thread (no `glReadPixels`, no
     `eglSwapBuffers`).
   - Drains controller-derived ImGui keys from the bridge each tick via the
     weak extern `gsTryConsumeXrImGuiKey()`.
   - Caps idle wait at 50 ms (`m_cv.wait_for(50ms)`) so the menu animates and
     queued key presses surface within ~50 ms even with no incoming video.

3. **Bridge** ([openxr_video_bridge.cpp](app/src/main/cpp/openxr_video_bridge.cpp))
   - Thread-safe glue: shared `EGLContext`, renderer's color texture id +
     dims, controller-key queue.
   - Exposes weak C-linkage hooks (`gsGetSharedEglContext`,
     `gsPublishRendererTexture`, `gsTryConsumeXrImGuiKey`) so the shared
     renderer can detect "we're on Quest" and route through the bridge
     without depending on Quest-specific headers.

There is no Android `SurfaceView`, no `SurfaceFlinger` interaction, no CPU
buffer round-trip. The user only ever sees the OpenXR composition layer.

## Controller mapping

| Quest Touch input | Key | Notes |
|---|---|---|
| Right A button | `G` | OSD action (e.g., GS record) |
| Right B button | `R` | OSD action (e.g., air record) |
| Index trigger (either) | `Enter` | menu confirm; threshold 0.5 |
| Thumbstick click (either) | `Enter` | menu confirm |
| Thumbstick direction (either) | arrow keys | threshold 0.6 |

The mappings live in `syncControllerInputs()` in
[openxr_quest_runtime.cpp](app/src/main/cpp/openxr_quest_runtime.cpp).
Bindings are declared for `/interaction_profiles/oculus/touch_controller`.
The app does not require a controller — input also reaches the GS via MCP
and any other paths the shared renderer supports.

## Deployment helpers

- `build.bat` — builds the debug APK.
- `install.bat` — installs the current debug APK to the primary user.
- `deploy.bat` — force-stops, rebuilds, installs, launches, and verifies the
  app is resumed.
- `enable_wifi_debugging.bat <QUEST_IP> [PORT]` — switches an authorized
  USB-connected Quest into ADB TCP mode and connects to it over Wi-Fi.
- `deploy_wifi.bat <QUEST_IP[:PORT]>` — same as `deploy.bat` against a
  specific Quest Wi-Fi ADB target.

## Third-party

See [`app/src/main/cpp/third_party/`](app/src/main/cpp/third_party/):

- `openxr/` — Khronos OpenXR loader headers.
- `devourer/{src,hal}` — RTL8812AU driver (raw broadcast / Wi-Fi scan).
- `include/libusb.h` + `prebuilt/arm64-v8a/libusb1.0.so` — USB host access.
