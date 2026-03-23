# Android GS

Initial Android ground-station scaffold for this repository.

Current scope:

- standalone Android app project
- Kotlin + Compose app shell
- NDK/CMake native library
- reusable GS core linked into Android native code
- minimum Android version: 6.0 (API 23)
- compile/target SDK: 34 for compatibility with the current Android Gradle plugin

Not implemented yet:

- transport
- packet ingress
- JPEG frame decode path
- OSD rendering
- settings/control UI
