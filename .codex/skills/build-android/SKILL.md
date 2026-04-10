---
name: build-android
description: Build the Android gs target via Gradle.
---

# Build Android GS

Build the Android target using this exact PowerShell command:

```powershell
powershell -Command "Set-Location 'D:\Github\esp32-cam-fpv\esp32-cam-fpv\android_gs'; .\gradlew.bat assembleDebug"
```

Rules:

- Use PowerShell with `Set-Location` and `.\gradlew.bat`; do not use `cmd /c "cd ... && gradlew.bat"` because `cmd` can fail to find `gradlew.bat` after `cd`.
- Do not pass the path as an argument to `gradlew.bat`; Gradle must run from within the `android_gs` directory.
- Run the command, report whether the build succeeded or failed, and surface compiler/linker errors clearly on failure.
