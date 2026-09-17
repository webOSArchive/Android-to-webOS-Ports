# apkenv environment variables

Every `APKENV_*` environment variable the apkenv sources read at run time. Paths are relative to
`apkenv/`. Compile-time `-D` macros that share the prefix (`APKENV_STATIC_MODULES`,
`APKENV_LOCAL_BIONIC_PATH`, `APKENV_DEFAULT_APK`, `APKENV_GLES2`, header guards, ...) are not
environment variables and are left out.

**Where values come from.** A variable is read from the process environment. On webOS, a packaged
launch (the binary runs from `/apps/usr/palm/applications/...`) also loads
`<appdir>/android/apkenv.env`, a file of `KEY=VALUE` lines (`#` comments and blank lines are
skipped), before anything else runs. The loader is in `apkenv.c:788-806`. It calls
`setenv(key, value, 0)`, so a variable already in the environment wins over the file. The launcher
execs the binary without a shell, so this file takes the place of a `play.sh`.
`packaging/build-ipk.sh` copies `packaging/<game>/apkenv.env` into the package.

**Rule:** a shipped `apkenv.env` carries launch settings only. It never carries debug, trace or
autotap variables. Those are for development runs.

Unless a row says otherwise, a flag is read once and cached. "`=1`" means the value must start with
`1`, and "set" means any value turns it on.

## Launch settings used by shipped packages

From the committed `packaging/*/apkenv.env` files.

| Variable | Used by |
|---|---|
| `APKENV_HOST_MONO` | Aralon, RoboCop, Temple Run 2 |
| `APKENV_PTHREAD_STACK_CLAMP` | Aralon, Dead Space, RoboCop, Tiny Death Star |
| `APKENV_SPLASH_RGB` | Temple Run 2, Tiny Death Star |
| `APKENV_SPLASH_SIZE` | Temple Run 2, Tiny Death Star |
| `APKENV_FMOD_MUSIC_PCM` | Temple Run 2 |
| `APKENV_FMOD_MUSIC_GAIN` | Temple Run 2 |
| `APKENV_FMOD_MUSIC_PREF` | Temple Run 2 |
| `APKENV_UNITY_PORTRAIT` | Aralon, RoboCop |
| `APKENV_UNITY_SPLASH` | Aralon, RoboCop |
| `APKENV_UNITY_PACKAGE` | Aralon, RoboCop |
| `APKENV_UNITY_OBB` | Aralon, RoboCop |
| `APKENV_UNITY_OBB_CODEPATH` | RoboCop |
| `APKENV_UNITY_VERSION_NAME` | RoboCop |
| `APKENV_UNITY_VERSION_CODE` | RoboCop |
| `APKENV_BLAST_CONTENT` | Dead Space |
| `APKENV_BLAST_LOGICAL` | Dead Space |
| `APKENV_MARM_LOGICAL` | Plants vs. Zombies HD |
| `APKENV_MORTAR_DIALOG_BUTTON` | Fruit Ninja |

Where's My Water and Amazing Alex ship with no `apkenv.env`.

## Core: loader, host libraries, libc and pthread shims

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_HOST_MONO` | path to a glibc-built `libmono.so` (unset) | Bridges a native Mono in place of the apk's bionic `libmono.so` before any apk library loads. The path is relative to the run dir. Exits with status 3 if the bridge fails. | `apkenv.c:1026` |
| `APKENV_GLES_VERSION` | `1` or `2` (auto-detected from the imported GLES libs, or the module's preference) | Forces the GLES version of the context. | `apkenv.c:1234` |
| `APKENV_ASSET_ROOT` | directory (unset; `wheresmywater2` defaults it to `/media/internal/wmw2root/assets`) | When `fopen` misses an absolute path, retries it under this root. | `compat/libc_wrappers.c:327`, `modules/wheresmywater2.c:131` |
| `APKENV_PTHREAD_STACK_CLAMP` | `=1` (off) | Raises `pthread_attr_setstacksize` requests below 128 KB to 128 KB. Without it, glibc rejects FMOD's 8 KB stacks and the thread is never created. | `compat/pthread_wrappers.c:330` |
| `APKENV_SYS_CPU` | `=1` (off) | Answers `/sys/devices/system/cpu/{present,possible,online}`, which the webOS kernel lacks, from the real core count, through a file under `/media/internal/.apkenv/`. | `compat/libc_wrappers.c:405` |
| `APKENV_HOSTUI_FULLSCREEN` | `1` or `0` (windowed) | Makes the SDL2 host-UI window fullscreen. `platform/osmesa.c` sets it for its `hostui` child process, so it does not apply on webOS. | `hostui.c:228`, set in `platform/osmesa.c:518` |

## Display and GL

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_FBO_ROT` | `1` = 90° or `3` = 270° (`1`) | Rotation of the ES2 portrait-FBO present. Also flips the Unity tilt mapping. | `compat/fbo_es2.c:258` |
| `APKENV_WMW_FBO_ROT` | `1` or `3` (`1`) | The same rotation for the GLES1 render-to-FBO path (WMW). | `compat/gles_wrappers.c:634` |
| `APKENV_SPLASH_RGB` | path to raw RGB24, bottom row first (unset) | A host-drawn boot splash on the FBO present quad. It retires on the engine's first draw. | `compat/fbo_es2.c:177` |
| `APKENV_SPLASH_SIZE` | `WxH` (the FBO size) | The dimensions of the `APKENV_SPLASH_RGB` payload. | `compat/fbo_es2.c:178` |
| `APKENV_OPAQUE_PRESENT` | `=1` (off) | Forces framebuffer alpha to 1 before the swap. The webOS compositor blends the GL layer by alpha. | `platform/webos.c:396` |
| `APKENV_EGL_WARMUP` | `0` disables (on) | Initialises an EGL display and context before SDL does, so that SDL's ES2 request succeeds. | `platform/webos_egl_shim.c:318` |
| `APKENV_EGL_FIX` | `0`, `1` or `2` (`0`) | Rewrites SDL's EGL config choice. `1` filters the configs to ES2+window; `2` also chooses from the full config list. | `platform/webos_egl_shim.c:50` |

## Input and sensors

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_ACCEL_MAP` | `"x,y,z"` with permutation and sign, e.g. `"y,-x,z"` (identity) | Remaps the PDL accelerometer axes before they reach the module. | `platform/common/pdl_accelerometer_impl.h:54` |

## Audio (FMOD AudioTrack pump)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_FMOD_MUSIC_PCM` | path to raw S16LE stereo PCM at the mix rate (unset) | A looping music bed mixed into the FMOD pump underneath the engine. This works around Temple Run 2's stalled stream. | `audio/fmod_pump.c:150` |
| `APKENV_FMOD_MUSIC_GAIN` | 0.0-1.0 (0.5) | The base gain of the music bed. Only read when a PCM file is set. | `audio/fmod_pump.c:161` |
| `APKENV_FMOD_MUSIC_PREF` | PlayerPrefs float key, e.g. `TR Music Volume` (unset) | Each `SetFloat` on this key, and the stored value at startup, scales the music bed live. `0` means off. | `modules/unity.c:279` |

## Unity module (`modules/unity.c`)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_UNITY_PACKAGE` | package name (`com.unity3d.player`) | The value `getPackageName()` and friends return. | `modules/unity.c:1908` |
| `APKENV_UNITY_OBB` | OBB path, relative to the run dir (unset) | Made absolute and passed to `nativeFile()` as the expansion file. | `modules/unity.c:1933` |
| `APKENV_UNITY_OBB_CODEPATH` | `=1` (off) | Makes the OBB the package code path and skips `nativeFile(apk)`, as Glu's launcher does. | `modules/unity.c:1919` |
| `APKENV_UNITY_PORTRAIT` | `0` = landscape (on) | Presents a 768x1024 portrait surface through the rotated FBO. | `modules/unity.c:816` |
| `APKENV_UNITY_SPLASH` | int (`1`) | The `splashMode` argument of `nativeInit(glesMode, splashMode)`. | `modules/unity.c:1989` |
| `APKENV_UNITY_GLES_MODE` | int (`2`) | The `glesMode` argument of `nativeInit`. | `modules/unity.c:1988` |
| `APKENV_UNITY_GLES2` | `0` disables (on) | The verified runtime patch that forces libunity (3.5) onto its ES2 device when the context is ES2. | `modules/unity.c:1783` |
| `APKENV_UNITY_AUDIO` | `0` disables (on) | Starts the FMOD AudioTrack pump. | `modules/unity.c:2102` |
| `APKENV_UNITY_VERSION_NAME` | string (`1.0`) | The `GetVersionName` answer. | `modules/unity.c:1195`, `:1411` |
| `APKENV_UNITY_VERSION_CODE` | int (`1`) | The `GetVersionCode` answer. | `modules/unity.c:1199` |
| `APKENV_UNITY_TILT_ROW` | 0-3 (portrait `3`, landscape `0`) | Selects the accelerometer-to-`nativeSensor` axis row. `/media/internal/apkenv-tilt.conf` overrides it. | `modules/unity.c:2280` |
| `APKENV_UNITY_TILT_INVX` | `0` or non-`0` (`0`) | Inverts the tilt X axis. | `modules/unity.c:2281` |
| `APKENV_UNITY_TILT_INVY` | `0` or non-`0` (`1`, or `0` when `APKENV_FBO_ROT=3` in portrait) | Inverts the tilt Y axis. | `modules/unity.c:2282` |
| `APKENV_UNITY_AUTOTAP` | `"frame:x:y[,...]"` (unset) | Diagnostic: synthetic tap, down at `frame` and up 6 frames later, through the real input path. | `modules/unity.c:2182` |

## Cocos2d-x module (`modules/cocos2dx.c`)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_COCOS_DEFAULT_FONT` | font path or name (Prelude-Medium, then `ArialUnicode.ttf`) | The first choice for the FreeType fallback typeface. | `modules/cocos2dx.c:248` |
| `APKENV_COCOS_LANDSCAPE` | `=1` (portrait FBO) | Turns off the 768x1024 rotated FBO, for comparison. | `modules/cocos2dx.c:887` |
| `APKENV_COCOS_NO_PACING` | `=1` (pacing on) | Turns off the sleep to the animation interval after each frame. | `modules/cocos2dx.c:1160` |
| `APKENV_COCOS_AUTOTAP` | `"x,y@frame;x1,y1>x2,y2@frame"` in panel pixels (unset) | Diagnostic: synthetic taps and drags. | `modules/cocos2dx.c:1014` |

## EA BLAST module (`modules/eablast.c`)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_BLAST_CONTENT` | dir, relative to the app dir (`android/extras`) | The content root. The module `chdir`s into it and reports it as external storage. | `modules/eablast.c:721` |
| `APKENV_BLAST_LOGICAL` | `WxH` (full screen) | Letterboxes a surface of that aspect, centred, and shifts the viewport and touch to match. | `modules/eablast.c:693` |
| `APKENV_BLAST_LIB` | library name (`libDeadSpace`) | The library searched first for `JNI_OnLoad`. | `modules/eablast.c:610` |
| `APKENV_BLAST_TOTALRAM` | byte count as a string (`536870912`) | The `GetTotalRAM` answer. | `modules/eablast.c:378` |
| `APKENV_BLAST_AUTOTAP` | `"x,y@frame;x1,y1>x2,y2@frame"` in screen pixels (unset) | Diagnostic: synthetic taps and swipes. | `modules/eablast.c:825` |

## Marmalade module (`modules/marmalade.c`, PvZ HD)

The offset defaults are for the PvZ HD `libpvz` build (md5 6b32d855).

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_MARM_LOGICAL` | `WxH` (full screen) | Letterboxes a surface of that aspect, centred, and shifts the viewport and touch to match. | `modules/marmalade.c:1540` |
| `APKENV_MARM_READSTRING` | string (`Player`) | Answers the engine's `getInputString` text prompt. | `modules/marmalade.c:565` |
| `APKENV_MARM_TICK` | `both`, `main` or `os` (`both`) | Which thread pumps `runOnOSTickNative`. | `modules/marmalade.c:243` |
| `APKENV_MARM_STACKSCAN` | `=1` (off) | Debug: logs the `deviceYield` call chain as `libpvz` offsets. | `modules/marmalade.c:272` |
| `APKENV_MARM_FIBREDUMP` | `=1` (off) | Debug: dumps the loader-fibre stacks. | `modules/marmalade.c:351` |
| `APKENV_MARM_RAMPATCH` | `=1` (off) | Experiment: a 1-byte runtime patch that forces the free-RAM gate branch. | `modules/marmalade.c:437` |
| `APKENV_MARM_RAMPATCH_OFF` | hex offset (`0x1f72b`) | The byte that `APKENV_MARM_RAMPATCH` patches. | `modules/marmalade.c:449` |
| `APKENV_MARM_RAMPATCH_VAL` | byte (`0xe7`) | The value that `APKENV_MARM_RAMPATCH` writes. | `modules/marmalade.c:450` |
| `APKENV_MARM_GATEBYPASS` | `=1` (off) | Experiment: patches the loader-state gate function to `return 0`. | `modules/marmalade.c:438` |
| `APKENV_MARM_GATE_OFF` | hex offset (`0x1f690`) | The gate function that `APKENV_MARM_GATEBYPASS` patches. | `modules/marmalade.c:466` |
| `APKENV_MARM_RUN_OFF` | hex offset (`0x23a04`) | The `runNative` file offset used to find the library base, for the patch and coop-yield experiments. | `modules/marmalade.c:444`, `:1421` |
| `APKENV_MARM_COOPYIELD` | `=1` (off) | Experiment: replays the engine's suspend-count yield bracket when the render stalls. | `modules/marmalade.c:1416` |
| `APKENV_MARM_DEC_OFF` | hex offset (`0x26538`) | The `dec_and_signal` offset for `APKENV_MARM_COOPYIELD`. | `modules/marmalade.c:1422` |
| `APKENV_MARM_INC_OFF` | hex offset (`0x26568`) | The `inc` offset for `APKENV_MARM_COOPYIELD`. | `modules/marmalade.c:1423` |
| `APKENV_MARM_STATE_OFF` | hex offset (`0x71b28`) | The loader-state struct offset for `APKENV_MARM_COOPYIELD`. | `modules/marmalade.c:1424` |

## Mortar module (`modules/mortar.c`, Fruit Ninja)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_MORTAR_DIALOG_BUTTON` | `0` = negative, `1` = positive, `-1` = leave unanswered (`0`) | How the shim answers dialogs it cannot draw. | `modules/mortar.c:488` |
| `APKENV_MORTAR_LICENSED` | `=1` (off) | Calls `SetAppLicensed(1)` after `SystemInit`. | `modules/mortar.c:1089` |
| `APKENV_MORTAR_AUTOTAP` | `"x,y@frame;x1,y1>x2,y2@frame"` in screen pixels (unset) | Diagnostic: synthetic taps and swipes. | `modules/mortar.c:1120` |

## Where's My Water module (`modules/wheresmywater.c`)

Most of these switches are left over from the touch-capture investigation (`plan/WMW-PVZ-TRAIL.md`).

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_WMW_FBO` | `0` = legacy per-call rotation hooks (FBO on) | Renders to a 768x1024 portrait FBO. | `modules/wheresmywater.c:781` |
| `APKENV_WMW_MULTITOUCH` | `=1` (single-finger legacy path) | Aggregates touch per finger with real finger ids. | `modules/wheresmywater.c:762` |
| `APKENV_WMW_PACE` | `0` disables (on) | Frame-paced touch delivery: at most one transition per finger per frame. | `modules/wheresmywater.c:831` |
| `APKENV_WMW_UITHREAD` | `0` = inline | Dispatches touch on a separate UI thread, but only when `APKENV_WMW_PACE=0` and this is not `0`. | `modules/wheresmywater.c:838` |
| `APKENV_WMW_KILLTHIEF` | `0` disables (on) | Neutralises the full-screen PushButton that swallows in-level touch. | `modules/wheresmywater.c:836` |
| `APKENV_WMW_AUDIO` | `0` disables (on) | Starts the FMOD pump. | `modules/wheresmywater.c:932` |
| `APKENV_WMW_ACCEL` | `0` disables (on) | Feeds a fixed gravity vector to `accelerometerChanged` every frame. | `modules/wheresmywater.c:866` |
| `APKENV_WMW_GX` | float (`0.0`) | The X component of that gravity vector. | `modules/wheresmywater.c:868` |
| `APKENV_WMW_GY` | float (`-9.81`) | The Y component of that gravity vector. | `modules/wheresmywater.c:869` |
| `APKENV_WMW_ORIENT` | 0-3 (`-1` = leave alone) | Overrides `GameSettings::CurrentDeviceOrientation` before init and every frame. | `modules/wheresmywater.c:807`, `:870` |
| `APKENV_WMW_HOOK` | `0` disables (on) | Installs the inline logging hooks on the touch, widget and AABB functions. | `modules/wheresmywater.c:880` |
| `APKENV_WMW_FORCEINPUT` | `=1` (off) | Experiment: forces the widget-manager input gate on in-level. | `modules/wheresmywater.c:852` |
| `APKENV_WMW_CALLREGAIN` | `=1` (off) | Experiment: calls `Screen_WaterTest::regainedTop()` once after entering a level. | `modules/wheresmywater.c:854` |
| `APKENV_WMW_DOUBLEDOWN` | `=1` (off) | Experiment: dispatches BEGAN twice. | `modules/wheresmywater.c:856` |
| `APKENV_WMW_ENTERCATCH` | `=1` (off) | Experiment: sets `FingerCatcher+0xe0=1`. | `modules/wheresmywater.c:858` |
| `APKENV_WMW_FORCECATCH` | `=1` (off) | Experiment: calls `_acceptFinger` directly. | `modules/wheresmywater.c:860` |
| `APKENV_WMW_REHIT` | `=1` (off) | Experiment: re-invokes `touchDown` from `update()`. The comment marks it as failed. | `modules/wheresmywater.c:862` |
| `APKENV_WMW_REHIT2` | `=1` (off) | Experiment: re-invokes `touchDown` from inside the `touchDown` hook. | `modules/wheresmywater.c:864` |

## Where's My Water 2 module (`modules/wheresmywater2.c`)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_WMW2_FBO` | `0` = landscape surface (FBO on) | Renders to a 768x1024 portrait FBO. | `modules/wheresmywater2.c:145` |
| `APKENV_WMW2_MULTITOUCH` | `=1` (off) | Aggregates touch per finger. | `modules/wheresmywater2.c:134` |
| `APKENV_WMW2_ACCEL` | `0` disables (on) | Feeds a fixed gravity vector. | `modules/wheresmywater2.c:136` |
| `APKENV_WMW2_DPI` | float (`132`) | The DPI used to compute the physical mm size passed to `renderInit`. | `modules/wheresmywater2.c:199` |
| `APKENV_WMW2_AUDIO` | `0` disables (on) | Starts the FMOD pump. | `modules/wheresmywater2.c:213` |

## Debugging and tracing (never in a shipped env file)

| Variable | Values (default) | Effect | Read in |
|---|---|---|---|
| `APKENV_TRACE_FILES` | set, not starting with `0` (off) | Logs `[FILE]` for `open`/`fopen`/`stat`/`opendir` (failures always, the first 400 successes), `[MMAP]` for mmap, and `write` failures. | `compat/libc_wrappers.c:346` |
| `APKENV_TRACE_SEEK_RANGE` | `"0xLO-0xHI[,...]"`, up to 16 ranges (unset) | Logs seeks into each range and counts the bytes read in it. | `compat/libc_wrappers.c:32` |
| `APKENV_TRACE_FREE` | pointer, e.g. `0x...` (unset) | Logs the bionic-side caller of `free()` for that pointer (up to 4 hits). | `compat/libc_wrappers.c:1532` |
| `APKENV_TRACE_CALLS` | `"lib:sym,lib:sym,..."`, up to 16 (unset) | Registers logging hooks that forward 4 word arguments and log entry and exit. | `compat/hooks.c:464` |
| `APKENV_HOOK_DEBUG` | set (off) | Logs how hooks resolve for GL draw symbols, and checks the hook table at init. | `compat/hooks.c:145`, `:501` |
| `APKENV_PTHREAD_WATCH` | `=1` (off) | A watchdog that logs `[PWATCH]` for any wrapped blocking pthread call still blocked after about 2 s. | `compat/pthread_wrappers.c:105` |
| `APKENV_THREAD_SAMPLE` | set (off) | Every ~5 s, logs each thread's blocking syscall and state from `/proc/self/task`. | `audio/fmod_pump.c:291` |
| `APKENV_AUDIO_METER` | set (off) | Logs the FMOD pump's output level meter about every 2 s. | `audio/fmod_pump.c:149` |
| `APKENV_GL_DEBUG` | set (off) | `[GLPATH]` first-path marks, `[GLPROJ]` projection matrices, `[FBO2]`/`[FBO-OES]` framebuffer binding at present. | `compat/gles2_wrappers.c:538`, `compat/gles_wrappers.c:447`, `:532`, `:2188`, `compat/fbo_es2.c:359` |
| `APKENV_GL_PROBE` | `=1` (off) | `[GLPROBE]` per-frame ES1/ES2 draw and vertex counts, viewport, FBO and clear colour. Called from the Unity module. | `compat/gles_wrappers.c:379` |
| `APKENV_GL_UPLOADCHECK` | `=1` (off) | Calls `glGetError` around every texture and buffer upload in both wrapper tables, and returns drained errors to the engine. | `compat/gl_uploadcheck.c:32` |
| `APKENV_GL_SNAPSHOT` | `"frame[,frame...]"` (unset) | Writes `/media/internal/apkenv-snap-<n>.ppm` at those frames. `tools/grab.sh` is the on-demand alternative. | `platform/webos.c:355` |
| `APKENV_SDL_TRACE` | `0` disables (on) | `[SDLHB]` input heartbeat and `[SDLEV]` non-motion SDL events. | `platform/webos.c:189` |
| `APKENV_EGL_PROBE` | set (off) | Probes whether ES1/ES2 contexts can be created at three points in platform init. | `platform/webos_egl_shim.c:243` |
| `APKENV_ACCEL_DEBUG` | set (off) | Logs the raw PDL vector and the mapped vector. In Unity, also logs the `Input.acceleration` handed to the engine. | `platform/common/pdl_accelerometer_impl.h:84`, `modules/unity.c:2314` |
| `APKENV_MONO_TRACE` | Mono `--trace` spec, e.g. `N:Glu` (unset) | Passed to `mono_jit_set_trace_options`. Needs `APKENV_HOST_MONO`. | `apkenv.c:1038` |
| `APKENV_UNITY_ICALL_TRACE` | `=1` (off) | Logging trampolines on Unity's audio internal calls. Needs `APKENV_HOST_MONO`. | `compat/icall_trace.c:231` |

The autotap variables (`APKENV_UNITY_AUTOTAP`, `APKENV_COCOS_AUTOTAP`, `APKENV_BLAST_AUTOTAP`,
`APKENV_MORTAR_AUTOTAP`) and the Marmalade and WMW experiment switches are also diagnostic. They are
listed with their modules above.
