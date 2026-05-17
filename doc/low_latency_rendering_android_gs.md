# Low-Latency Rendering Pipeline for Android GS

This document describes the target low-latency rendering design for Android GS.

## Goal

The target behavior is:

- receive packets as fast as possible
- recover missing transport packets with FEC
- assemble a complete JPEG frame immediately after the last packet arrives
- hand the completed JPEG frame to a decoder thread immediately
- render the newest decoded frame on a native surface with minimum extra copies and minimum UI-thread work

The guiding rule is:

- freshness matters more than completeness of history

So the pipeline should prefer dropping stale work over building latency.

## Target Design

The intended Android GS pipeline is:

1. native RX thread
2. native FEC / transport decode stage
3. native frame assembly stage
4. native JPEG decode thread
5. native render thread
6. thin Android UI shell

The pipeline should be fully native from packet receive to frame presentation.

## Stage Breakdown

## 1. Native RX Thread

Responsibilities:

- own the UDP socket
- receive transport packets
- timestamp packet arrival if latency instrumentation is enabled
- forward packet payloads immediately into the transport/FEC stage

Requirements:

- elevated thread priority
- no rendering work
- no JPEG decode work
- minimal allocation pressure

The RX stage should be isolated so network receive is never blocked by decode or rendering.

## 2. Native FEC / Transport Decode Stage

Responsibilities:

- group packets by transport block
- recover missing source packets using FEC
- dispatch recovered payloads immediately when enough block data is available

This is the stage where FEC decoding belongs.

Requirements:

- work on recovered source payloads, not UI objects
- avoid any Java/Kotlin round-trips
- publish decoded payloads directly into the session/frame assembly stage

## 3. Native Frame Assembly Stage

Responsibilities:

- parse recovered video payloads
- assemble JPEG fragments into one contiguous JPEG frame
- detect frame completion when the last fragment arrives
- publish the completed JPEG frame immediately to the decoder thread

Design rule:

- once a frame is complete, hand it off immediately
- do not delay for UI refresh, stats work, or unrelated bookkeeping

This stage should use fixed buffers from a pool.

## 4. Native JPEG Decode Thread

Library:

- `libjpeg-turbo`

Responsibilities:

- wait for the newest completed JPEG frame
- decode JPEG into RGBA
- publish the decoded frame immediately to the render thread

Requirements:

- dedicated native thread
- use `libjpeg-turbo` through `turbojpeg.h`
- prefer fast decode flags suitable for live video
- do not create Java `Bitmap` objects

Recommended output:

- RGBA buffer in native memory

Recommended policy:

- if a newer JPEG frame arrives before decode starts, replace the older pending frame
- preserve low latency rather than decode every frame

## 5. Native Render Thread

Responsibilities:

- own the EGL/OpenGL context
- own the `ANativeWindow`
- upload the newest decoded RGBA frame to a GL texture
- draw video quad
- draw OSD quads
- draw OSD menu quads and text

The renderer should target an Android `SurfaceView`.

### Video Rendering

Video should be rendered as a textured quad using OpenGL.

Supported Android GS video modes:

- `Stretch`
- `Letterbox`
- `Zoom to Fill`

These modes should be implemented in GL coordinates and texture coordinates, not through Compose layout.

### OSD Rendering

The OSD font is a PNG atlas.

It should be rendered as textured quads in OpenGL, using the same general approach as the Linux GS:

- glyph atlas texture
- per-glyph quad generation
- CPU-side OSD layout
- GPU-side textured quad draw

### OSD Menu Rendering

The OSD menu should also be rendered in OpenGL.

Recommended primitives:

- colored quads for panels, highlights, borders, and backgrounds
- atlas-based text quads for all menu labels

Menu logic and layout may remain CPU-side, but drawing should happen in the same GL pipeline as video and OSD.

## 6. Thin Android UI Shell

Responsibilities:

- lifecycle
- permissions if needed
- input routing
- host `SurfaceView`

The UI shell should not perform video drawing.

It should not be the primary rendering path for:

- video frames
- OSD text
- OSD menu

## Buffering and Ownership Model

The target design should use fixed pools for:

- transport packet buffers
- assembled JPEG frame buffers
- decoded RGBA frame buffers

Between major stages, communication should use latest-frame mailboxes rather than deep queues.

Recommended stage handoff model:

- RX -> transport/FEC: packet queue or lock-light ring buffer
- frame assembly -> JPEG decode: latest completed JPEG mailbox
- JPEG decode -> render: latest decoded RGBA mailbox

## Frame Freshness Policy

For minimum latency:

- only the newest pending JPEG frame should matter for decode
- only the newest pending decoded frame should matter for rendering

This means:

- old incomplete work may be dropped
- old pending decode work may be replaced
- old pending render work may be replaced

The system should prefer:

- display the newest available frame

instead of:

- display every frame in order with accumulated delay

## Multicore Use

The target pipeline is explicitly multicore-friendly.

Suggested thread separation:

- core A: RX
- core B: FEC / transport / frame assembly
- core C: JPEG decode
- core D: render

Exact core pinning is device-dependent and not guaranteed on Android, but the software architecture should still separate these stages into independent threads so the scheduler can distribute them.

Key requirement:

- decode must start as soon as the completed JPEG stream is available

That means the frame assembly stage should publish the JPEG frame immediately to the decoder thread.

## Why `libjpeg-turbo`

`libjpeg-turbo` is the right decoder library for this design because it is:

- fast
- mature
- ARM-optimized
- SIMD-accelerated
- already used by Linux GS

It is the best practical JPEG decoder choice for Android GS while remaining on JPEG.

## Reference Model

Linux GS is the rendering reference for:

- atlas-based OSD rendering
- menu rendering concepts
- `libjpeg-turbo` JPEG decode usage

Relevant files:

- [gs/src/Video_Decoder.cpp](../gs/src/Video_Decoder.cpp)
- [gs/src/osd.cpp](../gs/src/osd.cpp)
- [gs/src/osd_menu.cpp](../gs/src/osd_menu.cpp)

Android GS target files:

- [android_gs/app/src/main/cpp/native_bridge.cpp](../android_gs/app/src/main/cpp/native_bridge.cpp)
- [android_gs/app/src/main/cpp/android_jpeg_decoder.cpp](../android_gs/app/src/main/cpp/android_jpeg_decoder.cpp)
- [android_gs/app/src/main/cpp/android_jpeg_decoder.h](../android_gs/app/src/main/cpp/android_jpeg_decoder.h)
- [android_gs/app/src/main/cpp/android_video_renderer.cpp](../android_gs/app/src/main/cpp/android_video_renderer.cpp)
- [android_gs/app/src/main/cpp/android_video_renderer.h](../android_gs/app/src/main/cpp/android_video_renderer.h)
- [android_gs/app/src/main/cpp/CMakeLists.txt](../android_gs/app/src/main/cpp/CMakeLists.txt)
- [android_gs/app/src/main/java/com/esp32camfpv/androidgs/MainActivity.kt](../android_gs/app/src/main/java/com/esp32camfpv/androidgs/MainActivity.kt)
- [android_gs/app/src/main/java/com/esp32camfpv/androidgs/NativeCore.kt](../android_gs/app/src/main/java/com/esp32camfpv/androidgs/NativeCore.kt)

Shared frame assembly:

- [gs/src/core/video_frame_assembler.cpp](../gs/src/core/video_frame_assembler.cpp)
- [gs/src/core/video_frame_assembler.h](../gs/src/core/video_frame_assembler.h)

JPEG library:

- [android_gs/third_party/libjpeg-turbo](../android_gs/third_party/libjpeg-turbo)

## Implementation Priorities

Recommended implementation order:

1. native `SurfaceView` + EGL/OpenGL renderer
2. native `libjpeg-turbo` decode
3. explicit latest-frame mailboxes between assembly, decode, and render
4. native RX thread
5. OSD atlas rendering in GL
6. OSD menu rendering in GL
7. stage-level timing and dropped-frame instrumentation

## Summary

The target Android GS rendering architecture is:

- fully native from receive to presentation
- JPEG decoded by `libjpeg-turbo`
- video rendered through OpenGL to a `SurfaceView`
- OSD font atlas rendered as textured quads
- OSD menu rendered in the same GL pipeline
- latest-frame pipeline optimized for minimum latency rather than frame preservation
