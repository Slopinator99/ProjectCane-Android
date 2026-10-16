<p align="center">
  <img alt="ProjectCane" src="README-assets/projectcane-logo.png" height="130" />
  &nbsp;&nbsp;&nbsp;
  <img alt="Android" src="README-assets/android-logo.png" height="130" />
</p>

# ProjectCane Android

An Android port of [ProjectCane](https://github.com/theclub654/ProjectCane), the native
PC port of the NTSC release of *Sly Cooper and the Thievius Raccoonus*.

This fork is based on upstream **0.4.1** and adds everything needed to run the game on
ARM64 Android phones and handhelds: an OpenGL ES 3.2 renderer path, SDL2-based window,
audio and input, an on-screen controller, and an importer that extracts the game data
from your own ISO directly on the device.

> [!IMPORTANT]
> This project does not include any of the original game's copyrighted data. You must
> provide your own legally obtained NTSC-U copy of the game (SCUS-97198) as an `.iso`.

> [!NOTE]
> The Android port was developed with AI assistance (Claude, driven by the repository
> owner). The underlying game port is the work of the upstream ProjectCane authors.

## Devices this was tested on

| Device | Processor | GPU |
| --- | --- | --- |
| AYN Thor | Qualcomm Snapdragon 8 Gen 2 | Adreno 740 |
| RG557 | MediaTek | Mali-G615 |
| POCO F6 Pro (earlier build) | Qualcomm Snapdragon 8 Gen 2 | Adreno 740 |

The game is fully playable on these. Anything in the same processor families (Snapdragon /
Adreno, MediaTek / Mali) should normally work too. It has **not** been tested on Samsung
Exynos processors.

The minimum is Android 10, ARM64 and a GPU exposing OpenGL ES 3.2 with enough vertex-stage
buffer bindings; the game shows a clear error at start-up if the GPU does not qualify.
Prebuilt APKs are not published yet; build from source (see
[Building](#building-the-android-app)).

## Features

- **Single universal APK.** The GPU is detected at start-up and the renderer picks the
  right path (Adreno, Mali, or the generic one).
- **On-device data import.** Pick your ISO and the app extracts and rebuilds the level
  data itself (about two minutes, about 4 GB free space needed). You can also copy a
  folder that was already extracted on a PC.
- **Controller support** through SDL: Bluetooth or USB pads, built-in handheld controls,
  rumble (the device vibrator is used when the pad has no motor of its own).
- **On-screen controller** in a PlayStation-style layout with an editor for moving and
  resizing every control, plus opacity and size settings. It hides itself when a physical
  pad is used.
- **Back button = Start** (opens and closes the pause menu).
- **Save backup and restore** from the Options menu: saves are exported as a ZIP to any
  location you pick in the system file picker, and restored from one the same way.
- Menus trimmed to options that make sense on Android.

## Installing

1. Install the APK (`adb install ProjectCane-release-signed.apk`, or open it on the device).
2. Copy your NTSC-U ISO to the device (for example into the Download folder).
3. Open ProjectCane, choose **Select PS2 ISO and extract**, and pick the ISO. Wait for the
   import to finish; the game starts by itself.

If you previously extracted the data on a PC, choose **Or: copy a folder already extracted
on a PC** and select the folder that contains `Worlds` and `Cutscenes`.

Game data and saves live in `Android/data/org.projectcane.cane/files/` (reachable over USB).
Data imported by an older ProjectCane Android version is detected and re-imported once,
because upstream 0.4.x needs levels in a new format.

## Controls

| Input | Action |
| --- | --- |
| Pad / on-screen buttons | Standard PlayStation layout (X jump, Square attack, and so on) |
| Back button | Start / pause menu |
| Options > Controls | Touch mode, opacity, size, **Edit Touch Layout**, button remapping for physical pads |

The on-screen controller always uses the default layout; remapping only affects physical pads.

## Building the Android app

Requirements: JDK 17 or newer, Android SDK platform 34, NDK r26 (26.3.11579264), CMake 3.22.1.

```bash
cd android
./scripts/setup-android.sh                 # clones SDL2 into android/third_party
../scripts/ci/build-ffmpeg-android.sh      # cross-builds the decode-only FFmpeg into android/third_party/ffmpeg
./gradlew :app:assembleDebug               # or :app:assembleRelease
```

Point Gradle at your SDK with `android/local.properties` (`sdk.dir=...`). Release builds are
unsigned unless you provide a keystore; see `android/app/build.gradle` and `.github/workflows`.

Technical notes (renderer fallbacks, the Adreno fix, the data pipeline, how upstream merges
are done) are in [`PORTING.md`](PORTING.md). Android-specific layout is described in
[`android/README.md`](android/README.md).

## Keeping up with upstream

This repository tracks `theclub654/ProjectCane`. Upstream changes are merged into the port
and re-tested on device; `PORTING.md` lists what usually needs attention (shader and buffer
layout changes, the data/extraction format, new window or input calls).

## Roadmap

Ideas, in no particular order (nothing here is promised):
- **Higher refresh rates:** offer 120/144 Hz in the Frame Rate option and ask Android to run at that rate.
- **Auto-save and faster resume** when leaving the app (a true emulator-style save state is a research item).
- **Dual-screen mode** for handhelds with two displays (for example the AYN Thor): game on one screen, controls or extras on the other.
- **iOS port** (exploratory, not started): see `PORTING.md` for what it would take.

## Troubleshooting

- **"Game data not found" after an update:** data from an older version needs re-importing once.
- **Reporting a problem:** open the in-game pause menu > Options > **Backup / Restore Saves**, press
  **Save debug log to a file**, and attach the zip to your issue. It contains device details, the game
  settings and the logs of the last launches (never your saves or game data). The same logs are in
  `Android/data/org.projectcane.cane/files/Logs/`. With a PC you can also use
  `adb logcat -s cane SDL AndroidRuntime DEBUG`.
- **Import fails:** check free space (about 4 GB) and that the file is the NTSC-U ISO.

## Credits

- **ProjectCane** by [theclub654](https://github.com/theclub654) and contributors: the game port
  this is built on.
- **SDL2**, **FFmpeg** and **FAudio**, used under their own licenses.
- The Android robot in the header is reproduced from work created and shared by Google and used
  according to the terms of the
  [Creative Commons 3.0 Attribution License](https://creativecommons.org/licenses/by/3.0/).
  Android is a trademark of Google LLC.

## Legal

ProjectCane is an unofficial fan project and is not affiliated with or endorsed by Sony
Interactive Entertainment or Sucker Punch Productions. *Sly Cooper* and related names and
assets belong to their respective owners. No original game data is distributed with this
repository or its releases.

This repository does not currently contain a license file, like the upstream project it is
based on; the code is shared for preservation and learning, and rights to the original game
remain with their owners.
