# Android GS

Current scope:

- standalone Android app project
- Kotlin + Compose app shell
- NDK/CMake native library
- reusable GS core linked into Android native code
- minimum Android version: 6.0 (API 23)
- compile/target SDK: 34 for compatibility with the current Android Gradle plugin

Deployment helpers:

- `build.bat` builds the debug APK.
- `install.bat` installs the current debug APK for Android primary user `0`.
- `deploy.bat` force-stops, rebuilds, installs for primary user `0`, launches, and verifies the app is resumed.

