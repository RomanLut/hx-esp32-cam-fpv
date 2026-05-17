android deployment rule:
- always use this exact sequence when deploying to the connected Android device:
  1. force-stop the app
  2. rebuild
  3. wait 1 second
  4. install to the primary Android user only with `adb install --user 0 -r ...`
  5. launch
  6. verify the app is actually running:
     - check `pidof com.esp32camfpv.androidgs`
     - check `dumpsys activity activities` shows `com.esp32camfpv.androidgs/.MainActivity` as resumed
     - do not report "started" unless both checks pass
- deployment steps 1-6 must be executed sequentially, never in parallel
- never run `install` and `launch` in parallel
- never use plain `adb install -r` for Android GS; on Samsung devices with Dual Messenger / Dual App profiles it can install or expose the APK for the secondary `DUAL_APP` user and create a second launcher icon
- if the package is already present in a Samsung Dual App user, remove only that secondary copy with `adb shell pm uninstall --user 95 com.esp32camfpv.androidgs`

android package/activity:
- package: `com.esp32camfpv.androidgs`
- activity: `com.esp32camfpv.androidgs/.MainActivity`

windows adb path:
- `D:\Android\android-sdk\platform-tools\adb.exe`

android install script:
- `android_gs\install.bat` installs `app-debug.apk` with `--user 0`
- `android_gs\deploy.bat` runs the full force-stop, rebuild, wait, primary-user install, launch, and verification sequence
