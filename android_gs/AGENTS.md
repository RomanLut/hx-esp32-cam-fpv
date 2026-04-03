android deployment rule:
- always use this exact sequence when deploying to the connected Android device:
  1. force-stop the app
  2. rebuild
  3. wait 1 second
  4. install
  5. launch
  6. verify the app is actually running:
     - check `pidof com.esp32camfpv.androidgs`
     - check `dumpsys activity activities` shows `com.esp32camfpv.androidgs/.MainActivity` as resumed
     - do not report "started" unless both checks pass
- deployment steps 1-6 must be executed sequentially, never in parallel
- never run `install` and `launch` in parallel

android package/activity:
- package: `com.esp32camfpv.androidgs`
- activity: `com.esp32camfpv.androidgs/.MainActivity`

windows adb path:
- `D:\Android\android-sdk\platform-tools\adb.exe`
