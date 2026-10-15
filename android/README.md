# ProjectCane for Android

Android app for ProjectCane (upstream 0.4.1 base). See the repository [`README.md`](../README.md)
for features, supported devices and installation, and [`../PORTING.md`](../PORTING.md) for the
technical notes.

## Requirements
- Android 10+ (API 29), ARM64, GPU exposing **OpenGL ES 3.2** with **>=10 vertex-stage SSBOs**
  (the game checks this at start-up and shows a clear error otherwise; GPUs without vertex-stage
  buffers use a built-in data-texture fallback).
- A gamepad is optional: an on-screen controller is drawn when no physical pad is in use.
- JDK 17+, Android SDK 34, NDK r26 (26.3.11579264), CMake 3.22.1.

## Build
```bash
cd android
./scripts/setup-android.sh                 # clones SDL2 into third_party/
../scripts/ci/build-ffmpeg-android.sh      # arm64 decode-only FFmpeg into third_party/ffmpeg
./gradlew :app:assembleDebug               # or :app:assembleRelease
```
`third_party/` and `local.properties` (`sdk.dir=...`) are local and not committed.

### FFmpeg (cutscenes)
`scripts/ci/build-ffmpeg-android.sh` cross-builds a minimal LGPL, shared, decode-only FFmpeg for
`arm64-v8a` into `third_party/ffmpeg` (`include/` and `lib/arm64-v8a/lib{avcodec,avformat,avutil,swscale}.so`).
The cutscene decoder is `Sly1-Release/fmv.cpp`.

## Game data
The app imports the data on the device: choose your NTSC-U ISO in the import screen (see the main README).
Alternatively extract on a PC with the File Extractor GUI and copy `Worlds/` and `Cutscenes/` to
`/sdcard/Android/data/org.projectcane.cane/files/`. You must own the game; no game data is included.

Upstream 0.4.x requires each level in the rebuilt PCBRX v17 format. The on-device importer does this with
`libbrxrebuild.so` (built from `tools/BrxRebuilder`, called through `extract/BrxRebuild.java`).

## Layout
- `app/` - Gradle app; `CaneActivity` extends SDL's `SDLActivity`; CMake builds `libmain.so` (the game) and
  `libbrxrebuild.so` (level rebuilder) from the repo root.
- `app/src/main/java/org/projectcane/cane/extract/` - on-device ISO extractor (port of the File Extractor GUI).
- `../platform/android/GLFW/` and `glfw_sdl.cpp` - the GLFW API implemented over SDL2 (window, GL ES context, pads).
- `../platform/android/touch_overlay.*` - on-screen controller and its layout editor.
- `../platform/gles_compat.h` - maps the few desktop-only GL calls to ES; force-included.
