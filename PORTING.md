# Android port – roadmap & status

Goal: run ProjectCane (Sly Cooper and the Thievius Raccoonus PC port) on Android phones and handhelds,
with a controller or the on-screen controller.

## Current status (upstream 0.4.1 base, 2026-10-15)
Playable on the AYN Thor (Snapdragon 8 Gen 2, Adreno 740) and the RG557 (MediaTek, Mali-G615); an earlier build ran on a
POCO F6 Pro. Not tested on Samsung Exynos. Done since the first light described below: a single universal APK
(GPU detected at start-up), the Adreno lighting-bake fix, on-device ISO import, menu cleanup for Android, Back = Start,
save backup/restore, an on-screen controller with a layout editor, device-vibrator rumble, and the merge of upstream
0.4.1 (see "Upstream 0.4.1 merge" at the end). The sections between here and "Updating from upstream" are the
original bring-up log and checklist and are kept as history; their unchecked items are mostly obsolete.

## Approach
Keep the desktop code path untouched. A `CANE_GLES` build mode (implied by Android) swaps:
| Concern | Desktop | CANE_GLES / Android |
|---|---|---|
| Window/context/input | GLFW, GL 4.3 core | SDL2 behind a GLFW-API shim (`platform/android/GLFW`), GL ES 3.2 |
| GL calls | glad desktop | same glad loader via `SDL_GL_GetProcAddress`; 7 desktop-only calls remapped in `platform/gles_compat.h` |
| Shaders | `#version 430/330 core` | version line rewritten to `#version 320 es` + default precisions at load (`glshaders.cpp`) |
| Particle base-instance draw | `glDrawElementsInstancedBaseInstance` | emulated by offsetting instance attribute pointers |
| `glPointSize` | fixed function | `u_pointSize` uniform in `blot.vert` (`#ifdef GL_ES`) |
| Audio | XAudio2 / FAudio | FAudio (already used on Linux), over SDL2 audio |
| Data/saves dir | exe dir / XDG | `SDL_AndroidGetExternalStoragePath()` |
| ImGui | GLFW + GL3 backends | stub GLFW backend (display size/time only) + GLES3 backend |

## Verified (on this repo snapshot, host machine)
- [x] `-DCANE_GLES=ON` configures, compiles (all 306 TUs) and links on x86-64 Linux with clang.
- [x] All 7 shader programs (`screen dysh glob celborder geom blot blip`) compile+link in a real
      **OpenGL ES 3.2** context (Mesa llvmpipe, SDL offscreen), after the version rewrite.
- [x] All game sources pass `aarch64-linux-gnu-g++ -fsyntax-only` (except `fmv.cpp`: FFmpeg headers absent on that machine).
- [x] `glob.geom` is an empty, unused stub – no geometry-shader dependency.
- [x] **Windows host, 2026-10-12:** NDK r26d (26.3.11579264) cross-compiles the full game + SDL2 2.30.11
      (add_subdirectory) + FAudio 25.02 + minimal LGPL FFmpeg n7.1 for arm64-v8a; `:app:assembleDebug`
      produces `app-debug.apk` (11.6 MB) containing `libmain.so`, `libSDL2.so`, `libFAudio.so`,
      `libc++_shared.so` and `libav{codec,format,util}.so`/`libswscale.so` under `lib/arm64-v8a/`.
- [x] FFmpeg cross-build script works from MSYS2 on Windows (host-toolchain auto-detection added).
- [x] **First light on device (2026-10-12), RG557 (Dimensity, Mali-G615 MC6, Android 14):** APK installs,
      `SDL_main` runs, GL ES 3.2 context created, shader pipeline reached. Logcat (tag `cane`):
      `OpenGL vendor: ARM / renderer: Mali-G615 MC6 / version: OpenGL ES 3.2 v1.r44p1…`.

### Device-run fixes made 2026-10-12 (all behind `__ANDROID__` or harmless on desktop)
- **Native stdout/stderr are invisible on Android** (go to `/dev/null`; SDL 2.30 has no stdio pump), so every
  fatal error (they go to `stderr`) looked like a silent clean exit. `main.cpp` now pipes stdout+stderr through
  a reader thread to logcat (tag `cane`), and `ReportRuntimeError` also logs synchronously via
  `__android_log_print` (the pump thread can lose the last lines when the process dies).
- **glad never loaded most ES 3.2 functions:** `find_coreGL()` (glad.c) parsed `"OpenGL ES 3.2 …"` as desktop
  `3.2`, so the `3.3`/`4.x` buckets were skipped → `glVertexAttribDivisor`, `glDrawElementsBaseVertex`,
  `glTexStorage2D`, … stayed NULL → SIGSEGV at first use (was hidden behind the SSBO gate). Fixed by treating
  ES ≥ 3.1 as 4.3 for bucket loading; symbols ES truly lacks stay NULL and are already remapped by
  `gles_compat.h`. Desktop GL version strings never match the ES prefixes, so behaviour there is unchanged.
- **Defensive:** `glfw_sdl.cpp::glfwGetProcAddress` now falls back to `dlsym(RTLD_DEFAULT, libGLESv2/v3.so)`
  if `SDL_GL_GetProcAddress` returns NULL (logs at WARN when it does).

### Hardware gate result on the RG557 (the current blocker)
`gl.cpp` refuses to start: **`GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0` (needs 10)**; combined SSBOs 35/11 OK,
SSBO bindings 70/14 OK, vertex outputs 128/48 OK, fragment/compute SSBOs 35 each. Confirmed by the driver:
bypassing the gate, `dysh.vert` link fails with *"The number of vertex shader storage blocks (1) is greater
than the maximum number allowed (0)"* — **Mali's compiler really rejects vertex SSBOs**, the limit is not
conservative. App now exits cleanly through the designed error path (message visible in logcat, no crash).
Shader inventory of the problem: `glob.vert` 10 blocks (9 readonly + writable `CACHEDLIGHTING`),
`celborder.vert` 5, `dysh.vert` 1 (all readonly except binding 7); `glob.frag` 1 (fragment stage is fine).

### Fallback-design limits captured on Mali-G615 (for §8.1 work)
`vertexUBO=36, combinedUBO=216, uboSize=65536 (64 KB), vertexTexUnits=16, maxTexSize=16384, vertexAttribs=32`
— plenty for moving the read-only blocks to UBOs (bindings 2,3,4,5,6,9,10,11,13) and handling
`CACHEDLIGHTING` (binding 7) with a per-draw recompute, per the brief's fallback plan.

### §8.1 vertex-SSBO fallback — implemented and verified on device (2026-10-12)
Approved by the user; the gate now falls back instead of failing when vertex-SSBO limits are short but
the replacement resources exist (≥8 vertex UBO blocks, ≥14 UBO bindings, ≥57136-byte blocks,
≥9 vertex texture units, ≥1024 max texel size). Logcat on the RG557:
`[Graphics] GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0 (< 10); engaging the vertex-SSBO fallback …` —
after that **all shader programs compile and link** (the previous hard failure was the `dysh.vert`
vertex-SSBO link error) and the main loop runs at a steady **60 fps** with no crash or GL-fatal output.

What the fallback does (flag `g_fVertexUboFallback`, define `CANE_VERTEX_UBO_FALLBACK` injected into
every shader by `ToGlslEs`; desktop path is untouched behind `#ifdef`):
- **Read-only light/shadow tables → std140 uniform blocks** at the same binding indices (2/3/4/5/13);
  the CPU layouts were already std140-compatible. `ACTIVELIGHTS`/`ACTIVESHADOWS` int-index arrays are
  uploaded in an `ivec4[64]` group layout (pack helpers in `light.cpp`/`shadow.cpp`); block buffers are
  allocated to their full declared size (`LIGHT lights[256]` = 53264 B, `SHADOW shadows[255]` = 57136 B).
- **Per-mesh arrays → RGBA32F/RGBA32UI data textures** sampled with `texelFetch`: warp state (unit 4),
  bones (5, mat4 stored 4-wide so `textureSize().y` is the exact count), pose deltas (6/7); cel-border
  edges (4), bone indices (6, usampler2D), weights (7), edge pose deltas (8). Access goes through
  `*_AT`/`*_LEN` macros that expand to the original SSBO expressions on the desktop path.
  Helpers `Create/Alloc/Update/DeleteFallbackTex` live in `gl.cpp` (save/restore `TEXTURE_BINDING_2D`
  around binds so render.cpp's per-unit cache stays coherent); `BindShaderStorageBuffer` routes
  indices 6/8/9/10/11/12 to their texture units and no-ops 7.
- **Writable `CACHEDLIGHTING` (7) removed:** `CacheWriteMaterial()` becomes a no-op and the `TRLK_Baked`
  path recomputes the static bake per draw under the define.
- Shader sources are regenerated into `embedded_shaders.h` at build time, so `.vert`/`.frag` edits
  take effect on every build.


### Build-system fixes made during the first Windows build (2026-10-12)
- `android/settings.gradle`: added `pluginManagement`/`dependencyResolutionManagement` repositories
  (AGP could not be resolved without them).
- `android/app/build.gradle`: `versionCode`/`versionName` used `(expr) as int`, which Groovy misparsed
  ("Value is null"); replaced with `?.toInteger() ?: 1`.
- `CMakeLists.txt` (inside the `CANE_GLES` branch): FAudio's `install(EXPORT)` requires SDL2 to be in an
  export set; SDL2 comes from `add_subdirectory` on Android, so generation failed. FAudio's install rules
  are now skipped via `CMAKE_SKIP_INSTALL_RULES` (this superbuild never installs FAudio).
- `scripts/ci/build-ffmpeg-android.sh`: NDK prebuilt dir now selected by `uname` (linux/darwin/windows),
  and `--host-cc=/usr/bin/gcc` is passed under MSYS2.

## Not verified yet (needs device)
- [x] Runtime on a real GPU: **verified on Mali-G615** — see "Device-run fixes", "Hardware gate result"
      and "§8.1 vertex-SSBO fallback" above. The gate falls back, every program links, the main loop
      presents at 60 fps.
- [x] **In-game on RG557 (2026-10-12, after `fix-controller-accelerometer` and `fix-fallback-texture-upload`):**
      graphics (character body, cel outlines), audio/music and gamepad controls confirmed working by the user.
      Music root cause was never isolated (logs not captured) and it now plays; revisit only if it regresses.
      Speed: 60 fps is the intended rate, so the "runs too fast" report is closed. Open: stray green strip on a pipe (check).
- [ ] Cutscene playback with the Android FFmpeg build (decoder set is minimal: mpeg2video + pcm_s16le/be).
- [x] FAudio on Android: audio plays on device. [ ] latency / reverb quality not yet assessed.
- [ ] ARM64 float behaviour: PS2 round-toward-zero / flush-to-zero (`ps2_float.h` has only an x86 MXCSR path;
      ARM uses `fesetround(FE_TOWARDZERO)` but no flush-to-zero). Compare physics determinism vs. x86.
- [ ] Android lifecycle (pause/resume with a live GL context), controller hot-plug, back button.
- [ ] On-device menu navigation with a gamepad (the game's debug/ImGui menus are not gamepad-driven).

## Known risks
1. ~~**Vertex-stage SSBOs** (needs ≥10)~~ — **resolved by the §8.1 fallback** (Mali reports 0 and its
   compiler enforces it; the gate now downgrades to std140 UBOs + data textures and the app runs).
   Watch for correctness differences once game data is on-device: the lighting cache is recomputed per
   draw instead of read back, and per-mesh arrays are fetched through `texelFetch`.
2. `glob.vert` *writes* an SSBO from the vertex stage (cached lighting) – some mobile drivers are weak here.
3. Performance: MSAA + stencil shadows + per-vertex lighting; expect to need a render-scale/MSAA setting.
4. 32-bit ARM is unsupported (decompiled game code assumes 64-bit).

## Next steps (in order)
1. Get an NDK build going (Android Studio) and fix whatever the real toolchain flags.
2. Run on a device; read the OpenGL/limits log lines printed at start-up (`gl.cpp`).
3. FFmpeg for Android, then cutscenes.
4. Float-mode parity check; add ARM flush-to-zero via FPCR if physics diverges.
5. Gamepad-first UX: boot straight into the game, hot-plug handling, pause on lifecycle events.

## Automation (GitHub Actions)
| Workflow | When | What |
|---|---|---|
| `.github/workflows/android.yml` → `portability` | every push/PR | `scripts/ci/check-host.sh`: shaders in a GLES 3.2 context + AArch64 syntax check of all sources (~3 min) |
| `android.yml` → `apk` | after `portability` | installs NDK 26, builds SDL2 + a minimal arm64 FFmpeg (cached), `gradle assembleDebug`; uploads the APK artifact. On a `v*` tag also builds/publishes a release APK |
| `.github/workflows/upstream-sync.yml` | Mondays + manual | merges `theclub654/ProjectCane` `master` into the `android` branch on a `sync/upstream-DATE` branch, opens a PR (CI runs on it) or an issue on conflict |

Release signing (optional, tags only): add repo secrets `ANDROID_KEYSTORE_B64` (base64 of a .jks),
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`. Without them the release APK is unsigned;
use the debug APK for testing.

Local: `scripts/ci/check-host.sh [--full]`. On Windows, the FFmpeg cross-build runs from MSYS2/bash
(`ANDROID_NDK_HOME` must point at an NDK); the APK builds with `android/gradlew.bat :app:assembleDebug`
(JDK 17–21; Gradle wrapper 8.9 is present in `android/`).

## Updating from upstream
Files the port changes in upstream code: `CMakeLists.txt`, `platform/platform.h`,
`Sly1-Release/{glshaders,gl,tv,main,mgv,render,light,shadow,glob,wr,alo}.cpp`,
`Sly1-Release/{glob,celborder,dysh}.vert`, `Sly1-Release/glob.frag`, `Sly1-Release/glob.h`,
`Sly1-Release/blot.vert`. Everything else is new (`platform/android/`, `platform/gles_compat.h`,
`android/`, `tools/`, `scripts/ci/`).
When the `portability` job fails after a merge:
- *undefined / null GL function at runtime or compile error on a `gl*` call* → add a mapping in `platform/gles_compat.h`.
- *`glfw*` link error* → implement it in `platform/android/glfw_sdl.cpp` (+ declare in `GLFW/glfw3.h`).
- *shader compile failure* → fix the GLSL so it is valid GLSL ES 3.20 (also keep `tools/shadertest.cpp::ToEs` identical to `ToGlslEs` in `glshaders.cpp`).
- *Windows-only code outside `#ifdef _WIN32`* → guard it.

## Session notes 2026-10-13 (RG557, Android 14)
- **On-device ISO extraction** (`ImportActivity`, `extract/*`): verified on the real NTSC-U ISO. Cutscene PSS files
  (up to ~350 MB) are demuxed through a 1 MB window and streamed to disk (video and audio written as `.part` files and
  committed together); the audio decode drops each intermediate copy. Host check: output byte-identical to the desktop
  File Extractor (map `.txt` files differ only in CRLF) at a 256 MB heap; the app runs with `largeHeap` (512 MB).
- **Sidebag bug (Android-only, fixed in `basic.cpp::EnsureBasicSidebag`).** The "corrupt sidebag" check required 16-byte
  alignment. libc++ puts a `make_shared` object after a 24-byte control block (8 mod 16), so every valid sidebag was
  rebuilt on each call and all but the last `addo` option on an object were lost. Symptom: vault dials turned on Down but
  not on Up (the dial script adds options 1174 = up and 1175 = down). The check is now 8-byte alignment. This was
  not vault specific; any script giving an object several options was affected.
- Controller: the RG557 pad reports the D-pad as `ABS_HAT0Y` only; SDL turns hat input into D-pad buttons, which works.
- App icon: adaptive icon from the Sly mask in `README-assets/projectcane-logo.png` (`res/mipmap-anydpi`, `drawable-nodpi`).
  Some launchers (Daijishō) cache the first icon seen for a package; a restart refreshed it.
- Recents/"vertical view" report could not be reproduced and is closed.
- Debug logging is gated by `CANE_DIAG` (ON for debug, OFF for release). Remaining always-on messages are real errors only.

## Session notes: lighting bake cache and the Adreno glitch (2026-10-14)

**Bake cache (vertex-SSBO fallback, Mali + Adreno).** The static lighting of a glob is captured once with a transform-feedback
pass (`BakeGlobSubLighting`, render.cpp) into the glob's cache buffer and read back as vertex attributes 6..8 by baked draws
(`glob.vert`: `caneBake`, `bakeAmbient/bakeMidtone/bakeLight`). Per-draw light lists (`relightHdr/relightIdx` in the RO record) and a
light data texture (unit 10) keep the remaining per-vertex work small. Result on the AYN Thor: steady 60 fps.

**Adreno one-frame glitch (fixed).** Symptom: for one frame the world is drawn from a shifted camera, objects go missing and skinned
meshes scatter; seen where the intro camera pans up from the French flag and on the title screen. Mali is unaffected. Cause:
transform-feedback (rasterizer-discard) draws interleaved with scene draws inside one render pass. Fix: `PreBakeAll`/`PreBakeGlob`
run every bake in a pre-pass at the start of `DrawSw`, after the shadow textures are bound and before the first scene draw;
`DrawGlob` then skips the bake. `PreBakeGlob` must set `glslRko = 1`, otherwise the shader skips the lighting and the cache is zeros
(scene turns dark). The camera block is plain uniforms in `glob.vert` under `CANE_VERTEX_UBO_FALLBACK` (not proven necessary).

**Debug-build tooling.** `debug.cane.*` live props (finish, skip, resh, raster, frag, vert, binning, capture, nobake, noprebake);
`debug.cane.capture N` writes N presented frames (480x270 RGBA) to `capture.bin` in the app dir, `-1` runs a flicker detector that
saves `flick_*.bin`. Logs (tag `cane`): [Frame], [Passes], [GL/frame], [BufferSubData], [Bake], [Relight], [Cap], [Flicker].

**Testing lesson.** Always check the app is alive (pidof / no `FATAL`) before trusting a device experiment, and that gradle really
rebuilt. A crashing build once looked like "no glitches".

**Fallback if the glitch ever returns:** ANGLE (OpenGL ES over Vulkan) bundled in the app, optionally with the Turnip Vulkan driver via
libadrenotools. Background: Adreno binning-pass vs tile-pass mismatch (Mesa freedreno issue 4337; Qualcomm forum thread on Adreno 740).

## Android menu cleanup (2026-10-14)
The pause menu (`prompt.cpp`, `resp.h`) already offers MSAA, Frame Rate (30/60 + the display's refresh rates), Resolution, Draw Distance,
Aspect Ratio, Fog, GUI Style/Scale, volumes, inversions and mappings. On Android (`#ifdef __ANDROID__`): Window Mode and VSync are left
out of the Video menu (`s_arespkVideoMenuAll` is filtered into `s_arespkVideoMenu` when the menu opens), Mouse Camera and
"Disable Controller Input When Unfocused" are left out of Controls, "Keyboard Mapping..." appears only when
`cane_android_has_hw_keyboard()` (glfw_sdl.cpp, JNI Configuration.keyboard) says so, and "Exit to Desktop" is labelled "Exit Game".
Verified on the AYN Thor by the user.

## Android lifecycle (2026-10-15)
Checked on the AYN Thor: sending the game to the background keeps the process, pauses the OpenSL audio player and drops CPU use to ~0 (SDL
blocks the loop); returning resumes the same process at 60 fps with a valid GL context. Back button: `SDL_HINT_ANDROID_TRAP_BACK_BUTTON=1`,
the shim latches `SDL_SCANCODE_AC_BACK` for a few polls and exposes it as the pseudo key `CANE_KEY_ANDROID_BACK` (= `GLFW_KEY_WORLD_2`,
glfw3.h shim); `JOY::Update` (Input.cpp, `#ifdef __ANDROID__`) maps it to `BTN_START`. Controller hot-plug is handled in `Handle()`
(SDL_CONTROLLERDEVICEADDED/REMOVED). Not done: autosave on background (saves stay manual memory-card style).

## Save backup / restore (2026-10-15)
Saves live in `<getExternalFilesDir>/Saves` (`slot1..6.sav`, `system_settings.dat`, see save.cpp); Android deletes that folder on uninstall.
`SavesActivity.java` (new, gamepad-focusable like ImportActivity) backs them up to a user-chosen zip (`ACTION_CREATE_DOCUMENT`, so cloud drives
work) and restores from one (`ACTION_OPEN_DOCUMENT`): entry names are whitelisted, each file must start with the `PCANESAV`/`PCANESYS` header and
be < 1 MB, the old `Saves` is renamed to `Saves.before-restore-<timestamp>`, and the process is killed afterwards because the game keeps saves in
memory. Entry point: Options menu entry `RESPK_BackupSaves` (resp.h, Android only) -> `cane_android_open_saves_screen()` (glfw_sdl.cpp, JNI) ->
`CaneActivity.openSavesScreen()`. Verified on the AYN Thor by the user.

## On-screen controller (2026-10-15)
`platform/android/touch_overlay.{h,cpp}`: a virtual gamepad exposed in the last joystick slot (`kVirtualPad` = `GLFW_JOYSTICK_LAST` in
glfw_sdl.cpp), so `JOY::Update` needs no touch code except one rule in Input.cpp (`#ifdef __ANDROID__`): the virtual pad wins while a finger is on
it (+10 activity) and is never made sticky; the real pad's +3 stickiness otherwise blocks it. SDL finger events (touch->mouse synthesis disabled)
feed `OnFinger`; a lifted finger is kept for one read so a tap between two frames still counts. Drawn with `ImGui::GetForegroundDrawList()` from
`cane_touch_draw()` (main loop, after `RenderMenuGui`). Layout follows the ARMSX2 default (screenshot taken from the device); each of the 12 groups
has its own offset/size (`place_<name>=dx,dy,scale` in touch_settings.cfg, units of a 1080 px screen height) edited in the layout editor.
Menu: `RESPK_TouchMode/Opacity/Size/Edit` in the Controls menu (Android only). Verified on the AYN Thor by the user.

## Upstream 0.5.0 merge (2026-10-22)
Merged upstream Beta 0.5.0 (`f77825d`, three commits after the 0.4.1 base `d8e34ca`) as `3145d57`. What it brings: PAL (SCES-509.17) and Japanese (SCPS-150.36) support, a
localisation/content layer (`content.cpp`, language entries 79..83 in `resp.h`), the PCBRX v19 unified container (the runtime still reads v17 and PAL v18, so existing NTSC-U data keeps
working and **no re-import is needed**), widescreen FMV presentation, music fixes (random stops, pause ducking), a camera black-screen trace, and a new `PalExtractor` native tool.
Conflicts resolved: CMake (rebuilder tools moved to `File Extractor GUI/NativeTools/`; the PAL extractor is a desktop tool and is excluded on Android), `glob.h/.cpp` (upstream now retains the edge
points for map export; kept together with our fallback data textures), `resp.h` (our Android response IDs renumbered after the language entries, now 84..94), `prompt.cpp`, `sound.cpp`, `transition.cpp`.
Behaviour changes upstream made that we made switchable (Options > Video, saved in `gui_proportions.cfg`): the PS2 interface now has a fixed 4:3 horizontal stretch (`gui_layout.h`,
`g_fGuiClassicProportions`) and cutscenes are zoomed to fill (`fmv.cpp`, `g_fCutsceneClassic`). Android defaults to the classic look; desktop keeps upstream's.
Not done: on-device import of PAL/Japanese discs (the native `PalExtractor` could be ported behind the Java importer, see the shared-C++ plan in the iOS roadmap).

## Upstream 0.4.1 merge (2026-10-15)
Merged on top of the port, then re-tested on the Thor and the RG557 (fresh on-device import, intro, Paris and gameplay,
sound, Back, touch, rumble). What it needed:
- **Level data is rebuilt.** 0.4.1 refuses plain decompressed levels ("level is not in the required PCBRX v17 format");
  upstream's desktop extractor runs `Sly1BrxRebuilder` on every level. On Android the same sources
  (`tools/BrxRebuilder`) are built as `libbrxrebuild.so` with `platform/android/brx_rebuild_jni.cpp` and called by
  `extract/BrxRebuild.java` after each level is decompressed. `ImportActivity.hasData()` treats older levels as "no data" so
  existing installs re-import once. Dialog/commentary WAVs are now always named `VAG_<id>_<sector>.wav`. The rebuilder
  output was compared with an MSVC build: identical except 6 bytes (float rounding in the DXT encoder).
- **GLES cannot `glGenerateMipmap` compressed textures.** Upstream stores some textures as BC1/BC3 with "generate mips";
  on Adreno the higher mips stayed undefined (noise on distant windows/roofs). Under `CANE_GLES`, `shd.cpp` decodes those
  textures to RGBA8 first.
- **Cel borders and pose weights.** Upstream moved the cel-border edge data to one `CELPOINT` buffer and the pose weights to
  an SSBO (binding 14). The vertex-SSBO fallback keeps data textures (edge positions / bone indices / weights, units 4, 6, 7,
  pose deltas 8) and uses `uniform float poseWeights[64]` instead of the SSBO (`glslPoseWeights`, `glslCelPoseWeights`).
- **Input.** Upstream reads pads through SDL directly. The on-screen controller is one more source in `JOY::Update`
  (`cane_touch_pad_state`), always uses the default button layout (saved remaps made for a physical pad must not scramble it),
  and the Back key sets Start. Rumble falls back to the device vibrator (SDL haptic) when the active input has no motor.
- **Shim additions** (`platform/android/glfw_sdl.cpp`, mostly no-ops): `glfwGetTime`, `glfwSetWindowPos`,
  `glfwIconifyWindow`, `glfwSetWindowFocusCallback`, `glfwGetMonitorName`.
- Known, harmless: one `OpenGL error 0x501 was pending before framebuffer resize` line at start-up (reported by new 0.4.1
  code, origin not tracked down).

## Debug log for bug reports
Every launch writes `Android/data/org.projectcane.cane/files/Logs/session-<time>.log` (last 5 kept): a header with app version,
device, Android/API, SoC, GLES version, RAM/storage, whether game data is present and how the previous runs ended (crash,
signal, ANR on Android 11+), followed by the app's logcat. Options > Backup / Restore Saves > **Save debug log to a file**
zips the header, the settings file and the logs (no saves, no game data) through the system file picker so it can be attached
to an issue. Code: `DebugLog.java`, `SavesActivity.java`.

## Adreno lighting bake: pre-pass details and developer switches (0.4.1-android.2)
- The pre-pass (`PreBakeAll`) runs `DrawGlob`'s own set-up in a bake-only mode (`DrawGlobImpl(prpl, true)`), so the bake sees the same
  bones, poses, warp state, materials, lights and shadows as the inline bake did. Blend state and the cache bookkeeping are left to the
  real draw.
- **The bake pre-pass runs in a render pass of its own** (default): `DrawSw` binds a 1x1 dummy framebuffer for `PreBakeAll()` and then
  restores the scene framebuffer and viewport, so on the tile-based Adreno the scene pass contains no transform-feedback draw at all.
  This is what removed the Hideout first-view flicker (objects baking for the first time while the camera turned toward the level map)
  without bringing back the original wrong-frame glitch. Inline first bakes (`debug.cane.bisect 16`) also remove the flicker but bring the
  glitch back in big levels (Paris). `debug.cane.bisect 8192` puts the pre-pass back into the scene pass.
  Ruled out while finding this: a `glFinish`/`glFlush` after the pre-pass (128/256), baking first-time globs twice (32), and inline first
  bakes limited to one pass (512 translucent, 1024 cutout, 2048 opaque; 2560 = opaque + translucent works but is the same trade-off as 16).
  A glob that appears in two packets in one frame is still baked inline.
- Title-screen glitch counts used to choose this (detector `debug.cane.capture -1`-style, 25 s, Thor): pre-pass off 3, first bakes inline 1,
  first bakes in the pre-pass 0 (but with the Hideout flicker). The two spikes near the start of the intro are scripted camera cuts and show
  up in every configuration.
- Switches readable on every build (set with `adb shell setprop`, read once at start-up):
  `debug.cane.noprebake 1` (bake inside the scene passes), `debug.cane.glerr 1` (log GL errors after the pre-pass and glob draws to the
  session log), `debug.cane.bisect <mask>` (1 no pose-weight upload, 2 no poses, 4 no cel borders, 16 first bakes inline; 32/128/256/512/1024/2048 experiments, see above; 4096 unused; 8192 no own render pass for the pre-pass).
- A native crash writes `Logs/crash-<pid>.txt` (signal, fault address, backtrace as `libmain.so+offset`); keep the unstripped
  `libmain.so` of every release to decode it with `addr2line`.

## Roadmap: iOS port (planned, not started)

**Targets:** iPhone and iPad, sideloaded (AltStore / Sideloadly from Windows, free Apple ID, re-signed every 7 days). No App Store route.
**Build constraint:** there is no Mac, so the iOS build runs on GitHub-hosted macOS runners (free for a public repository): CI builds an unsigned
`.ipa`, you sideload it. Every test is a CI round trip (build, download, sideload, run), so the plan front-loads work that can be checked on
Windows/Android and keeps device-only questions few and small. Crash and log output come from the app's own debug log (exported to the Files app)
and `idevicesyslog` on Windows; there is no live debugger.

What carries over: the game code, SDL2 (supports iOS), FAudio (SDL audio, CoreAudio underneath), FFmpeg (cross-build in CI), the C++ BRX rebuilder, the
on-screen controller (`platform/android/touch_overlay.*`, plain C++ over SDL touch events), the Adreno/Mali "portable shader" path (data textures
instead of vertex-stage buffers, `CANE_PORTABLE_SHADERS`), the GLFW-on-SDL shim (`glfw_sdl.cpp`; its JNI parts need iOS counterparts).

### Phases
0. **Shared code first (checkable on Windows/Android).** Move the Java-only import code (ISO extractor, level rebuild driver, cutscene art) into
   C++ behind a small platform interface; Android keeps working, iOS gets it for free. Split `platform/android` into a shared SDL part and
   Android-only (JNI) parts. Add `platform/ios` stubs for: toast, open-file picker, refresh rate, share/export of logs and saves.
1. **CI and a hello-world.** A macOS workflow that builds SDL2 + a triangle for iPhone/iPad, produces an unsigned IPA artifact, and an `idevicesyslog`
   recipe. Proves the pipeline (toolchain, signing via Sideloadly, running on a real device) before any game code is involved.
2. **Renderer on ANGLE/Metal.** iOS only offers OpenGL ES 3.0 (deprecated); the engine wants ES 3.2 features. ANGLE (ES on top of Metal) is the
   realistic route. Things to prove in a small spike: `glDrawElementsBaseVertex` (`EXT_draw_elements_base_vertex`), transform feedback bakes (ES 3.0,
   fine), `glDrawElementsInstancedBaseInstance`, `glMemoryBarrier` and the SSBO uses (`alo.cpp`, `glob.cpp`: the portable path already avoids vertex-stage
   SSBOs), MSAA renderbuffers, `glPolygonMode`/`glDrawBuffer` call sites that desktop GL allows but ES does not. Fallback if ANGLE cannot do it: a
   Metal backend for the renderer (large) or per-draw emulation of the missing calls.
3. **Game on iOS.** Data import via the Files picker (ISO is about 4 GB: stream it, never load it whole), extraction and BRX rebuild into the app's
   Documents folder, FFmpeg build for cutscenes, audio, GameController input through SDL (MFi / Backbone pads), on-screen controller, haptics
   (CoreHaptics through SDL), safe areas (notch / Dynamic Island / home indicator) and ProMotion 120 Hz (`CADisableMinimumFrameDuration`).
4. **Polish and docs.** App icon, background/foreground handling (state save on `SDL_APP_WILLENTERBACKGROUND`, already wired), saves backup/restore through
   the Files app, README section with the sideloading steps and the 7-day re-sign caveat.

### Risks
- ANGLE-on-iOS feature gaps (see phase 2); building ANGLE itself is heavy (depot_tools, gn), so prefer a prebuilt xcframework if a trustworthy one exists.
- No debugger or GPU profiler: shader/GL errors must be logged in-app (the `debug.cane.glerr`-style switches exist for Android; mirror them).
- CI minutes and the slow feedback loop; mitigate by batching changes per run and keeping the shared code testable on Android.
- Apple restrictions on JIT/dynamic code are not a problem here (no JIT), but the 7-day free-account signing and the 3-app limit are real.

## Roadmap: integrated updater (idea, not started)
- **Check:** `GET https://api.github.com/repos/Slopinator99/ProjectCane-Android/releases/latest` (public, no token); compare `tag_name` against
  `BuildConfig.VERSION_NAME`. The app currently declares no `INTERNET` permission, so this is the first network access: make it opt-in (an Options entry,
  off by default, plus a "Check now" button) and say so in the README.
- **Download:** the `.apk` and `.apk.sha256.txt` assets into the app cache; verify the SHA-256 before installing.
- **Install:** hand the APK to Android's `PackageInstaller` (needs `REQUEST_INSTALL_PACKAGES`; the user grants "install unknown apps" for the app once and
  confirms each install). Updates keep saves and data because the release key stays the same; Android refuses an APK signed with a different key.
- **Open questions:** where the UI lives (the Java import/saves screens rather than the in-game menus), rate limiting (one check per day, `ETag`), and what to
  do on devices where installing from the app is blocked (fall back to opening the Releases page in the browser).

## Adreno rendering notes (what Qualcomm's own guidance says, and how it maps to this renderer)
Source: Qualcomm's Adreno documentation ([overview](https://docs.qualcomm.com/doc/80-78185-2/topic/overview.html), [mobile best practices](https://docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html),
[Game Developer Guides](https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/snapdragon-game-toolkit/gdg/gpu/best_practices.html)). None of it mentions transform feedback, so our bake path is outside the documented happy path.
- **Buffer updates mid-frame.** "Batch VBO updates before any draw that uses the modified region, otherwise the driver may keep multiple copies of the whole VBO."
  The transform-feedback bake rewrites a glob's cache buffer between draws, which is exactly this case (the driver renames/copies the buffer). The newer-driver crash
  (memcpy with a null pointer inside the draw, first frames after a load, relight globs) fits a copy of a buffer that was allocated with `glBufferData(nullptr)` and never
  written. Fix: allocate the cache zero-filled (`CaneAllocZeroedBuffer` in glob.cpp).
- **FlexRender direct mode.** Many texture fetches in vertex shaders per vertex, few vertices/draws, or tessellation/geometry shaders push the driver into direct
  (non-binning) rendering, which is often slower. The portable shader path fetches pose weights, cel borders and baked lighting from data textures in the vertex
  shader, so this is a likely contributor to the Adreno slowdowns seen earlier. Measure with Snapdragon Profiler before and after trimming vertex-stage fetches.
- **Storage buffers.** Prefer vertex buffers or uniform buffers; for larger read-only data prefer textures over SSBOs. (Matches why the vertex-SSBO fallback exists.)
- **Render passes.** Minimise passes, invalidate attachments you do not need (`glInvalidateFramebuffer`, `EXT_discard_framebuffer`, or a `glClear`) so no GMEM loads/stores
  happen. The bake pre-pass in its own FBO is fine only because nothing has been drawn into the scene pass yet.
- **Concurrent binning** is blocked by chains of dependent passes and by reusing one depth buffer with clears.
- **Other:** avoid `glFenceSync`/`glClientWaitSync`; cache linked programs with `glGetProgramBinary`/`glProgramBinary` (fewer first-frame stalls); `KHR_no_error` for shipping
  builds; Qualcomm itself says Vulkan is usually faster and easier to debug than OpenGL ES on Adreno.
- **Tools:** Snapdragon Profiler (Snapshot mode steps through a captured frame draw call by draw call; real-time counters), Android GPU Inspector.
- **Candidate redesigns of the bake** (none done): relight in the vertex shader every draw (`debug.cane.nobake` already does this for A/B), bake with a compute shader,
  or bake into a float texture read with `texelFetch`. Each removes transform feedback from the frame.

## High refresh rates (0.4.1-android.2+)
`glfwGetVideoModes` (platform/android/glfw_sdl.cpp) asks `CaneActivity.supportedRefreshRates()` for the rates the screen supports at its current
resolution, so the Frame Rate option (`SupportedFrameRates` in prompt.cpp) lists them. When the game's frame-rate target changes, `FramePacer::Wait`
calls `cane_android_request_refresh_rate`, which makes `CaneActivity.requestRefreshRate()` set `preferredDisplayModeId` to the lowest supported mode
at or above the target (Android otherwise tends to keep apps at 60 Hz). Verified on the AYN Thor (60/120 Hz panel): the FPS overlay reports 120.
