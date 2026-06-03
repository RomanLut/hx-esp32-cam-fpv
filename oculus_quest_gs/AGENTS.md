deployment rule (Quest):
- always use this exact sequence when deploying to a connected Quest:
  1. force-stop the app
  2. rebuild
  3. install with `adb install -r ...`
  4. launch
  5. verify the app is actually running:
     - check `pidof com.esp32camfpv.questgs`
     - check `dumpsys activity activities` shows `com.esp32camfpv.questgs/.MainActivity` as resumed
     - do not report "started" unless both checks pass
- deployment steps 1–5 must be executed sequentially, never in parallel
- never run `install` and `launch` in parallel
- the Quest does not have Samsung Dual App profiles, so plain `adb install -r` is fine

package / activity:
- package: `com.esp32camfpv.questgs`
- activity: `com.esp32camfpv.questgs/.MainActivity`

windows adb path:
- `C:\Users\roman\AppData\Local\Android\Sdk\platform-tools\adb.exe`

deployment scripts:
- `oculus_quest_gs\install.bat` installs `app-debug.apk`
- `oculus_quest_gs\deploy.bat` runs the full force-stop, rebuild, install, launch, verify sequence
- `oculus_quest_gs\enable_wifi_debugging.bat <QUEST_IP> [PORT]` switches a USB-connected Quest to ADB TCP and connects via Wi-Fi
- `oculus_quest_gs\deploy_wifi.bat <QUEST_IP[:PORT]>` runs the deploy sequence against a Wi-Fi ADB target

architecture notes:
- there is no `SurfaceView` and no `SurfaceFlinger` interaction; the renderer's
  EGL surface is a 16×16 pbuffer used only to satisfy `eglMakeCurrent`
- all rendering goes into a 1280×720 offscreen FBO whose color texture is
  shared (EGL share group) with the OpenXR thread; the OpenXR thread samples
  that texture each frame into the swapchain image of an
  `XrCompositionLayerQuad` (VIEW space, head-locked)
- the renderer thread runs `m_cv.wait_for(50ms)` so the UI ticks at ≥ 20 FPS
  even without video — controller key edges surface within ~50 ms
- the OpenXR thread's `EGLContext` must exist before the renderer creates its
  context (share group setup); `android_surface_backend.cpp::initEgl` polls
  the bridge for up to 2 s to handle the cold-start race
- the legacy `android_gs/` build still uses the original `SurfaceView` /
  `eglSwapBuffers` path; nothing in `components_gs/shared/` is unconditionally
  Quest-only — Quest behavior is gated on
  `gsPublishRendererTexture != nullptr`

controller input:
- Quest controllers do NOT reach `dispatchKeyEvent` while OpenXR has focus
  (`mCurrentFocus == null` per `dumpsys window`); never add controller
  mapping to Kotlin or to `androidKeyCodeToImGuiKey`
- controller input lives in `syncControllerInputs()` in
  `openxr_quest_runtime.cpp` and goes through the OpenXR action system
- ImGui keys queued from the OpenXR thread reach the renderer via the weak
  extern `gsTryConsumeXrImGuiKey()` drained inside `drawOverlayLocked()`

debugging:
- `OpenXR: blit pipeline ready` — XR shader/VBO created
- `OpenXR: action set attached (touch_controller)` — controller bindings live
- `renderer: offscreen FBO ready 1280x720 tex=N` — shared FBO created and
  texture id N published to the bridge
- `OpenXR: session state=5` — session reached `XR_SESSION_STATE_FOCUSED`
- if the renderer ticks at ≪ 20 FPS, suspect either the share-context wait
  in `android_surface_backend::initEgl` or that something has reverted to a
  window-surface path — both should not happen in the Quest build
