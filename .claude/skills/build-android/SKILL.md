---
name: build-android
description: Build the Android gs target via Gradle
allowed-tools: Bash
---

Build the Android target using this exact PowerShell command:

```
powershell -Command "Set-Location 'D:\Github\esp32-cam-fpv\esp32-cam-fpv\android_gs'; .\gradlew.bat assembleDebug"
```

Rules:
- Use PowerShell with `Set-Location` + `.\gradlew.bat` — do not use `cmd //c "cd ... && gradlew.bat"` because cmd fails to find gradlew.bat after cd
- Do not pass the path as an argument to gradlew.bat directly — Gradle must run from within the `android_gs` directory
- Run the command, show the full output, and report whether the build succeeded or failed
- On failure, show the linker/compiler errors clearly
