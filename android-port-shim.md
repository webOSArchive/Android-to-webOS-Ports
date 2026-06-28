# android-port-shim.md — running Android NDK games natively on webOS (no ACL)

A field guide for porting **Android NDK games** to the **HP TouchPad / Palm webOS** with a
**slim per-game wrapper** instead of the full Android Compatibility Layer (ACL). ACL is a whole
Android 2.3.6 userland (huge on disk + RAM, always running once installed). For a self-contained,
era-appropriate NDK game, a thin shim is enough.

Worked example in this guide: **Disney "Where's My Water?"** (`com.disney.WMW`, Walaber engine,
GLES1). Status: **boots, renders full-screen upright portrait, stable (0 crashes); menus are fully
touch-interactive (navigate, launch a level).** Active-gameplay touch (carve-by-drag + in-game HUD)
is the one open item (§6c). Sound and `.ipk` packaging not started. This already proves the
architecture end-to-end for a closed bionic/NDK game: a thin shim, no ACL, runs a real Android game
on webOS — booting, displaying correctly, and menu-playable.

---

## 1. Architecture: apkenv as the base

Base is **apkenv** (github.com/thp/apkenv) — runs Android NDK games on glibc ARM Linux:

- **bionic linker-as-library** loads the game's bionic `.so`s into the glibc process. Bionic runtime
  libs (libc/libm/libstdc++/…) are harvested from ACL's `system.img` and shipped under `libs/webos/`.
- **fake-JNI** — a hand-rolled `JNIEnv`/`JavaVM` good enough for NDK games (NewStringUTF, array
  alloc/region, method-call stubs). The game's Java is *not* run; a per-game **module** calls the
  game's `native` entrypoints directly.
- **platform backend** (`platform/webos.c`) — SDL 1.2 + GLES via PDL, audio, input, lifecycle.
- **per-game module** (`modules/<game>.c`) — looks up the game's JNI entrypoints and drives them
  (init → resize → resume → per-frame draw; routes input/lifecycle).

### Main loop (apkenv.c)
```
platform->input_update(module)   // SDL events -> module->input(...)
module->update(module)           // calls the game's per-frame native (render + logic)
platform->update()               // SDL_GL_SwapBuffers
```

---

## 2. Toolchain: two-compiler cross-build (critical)

The device is **glibc 2.4 / GCC 4.3.3** (PalmPDK). Modern code won't compile with 4.3.3 (dup GLES
typedefs) and won't *load* if linked against new glibc.

- **Compile** with modern `arm-linux-gnueabi-gcc-13` BUT force it onto PalmPDK glibc headers:
  `-nostdinc -isystem <gcc13 builtin include> -isystem /opt/PalmPDK/arm-gcc/sysroot/usr/include
  -I/opt/PalmPDK/include …`. gcc-13 tolerates the duplicate GLES1/GLES2 typedefs; forcing old
  headers avoids `__isoc23_*` / `GLIBC_2.38` symbol refs.
- **Link** with PalmPDK `arm-none-linux-gnueabi-gcc` (4.3.3) so symbols bind to glibc 2.4.
- Old-glibc extern-inline dup-symbol errors (`ferror_unlocked`) → `-fgnu89-inline`.
- Device GLES1 lib is the **old name `libGLES_CM.so`** (not libGLESv1_CM).

---

## 3. Picking & wiring a game module

- List the game's JNI exports: `readelf -sW lib*.so | grep Java_`. WMW had exactly:
  `WMWRenderer_{rendererInit,rendererResized,notifyScreenResized,rendererDrawFrame,
  rendererReloadContextData,rendererTouchBegan/Moved/Ended}` and
  `BaseActivity_{onGamePause,onGameResume,onLostFocus,onRegainedFocus,accelerometerChanged,
  backKeyPressed}`. **No separate update/step method → `rendererDrawFrame` does logic + render.**
- Verify native signatures by **disassembling** the function and watching which JNI vtable calls it
  makes (vtable byte offsets: GetIntArrayElements=0x2ec, GetFloatArrayElements=0x2f4). WMW's
  `rendererTouchBegan(JNIEnv*,jobject, jint count, jfloatArray xs, jfloatArray ys, jintArray ids)`
  was confirmed this way (2× GetFloatArrayElements + 1× GetIntArrayElements; stack args land at
  `sp+152/156` after `push {9 regs}` + `sub sp,#116`).
- Module init order that worked: `JNI_OnLoad → rendererInit(apkPath, dataDir, ctx) →
  rendererResized(w,h) → onGameResume → onRegainedFocus`. sourceDir = the apk path (engine reads
  assets straight from the zip); dataDir = a writable home dir.

---

## 4. The crash class: std::string(NULL) during scene build

WMW's "Lite" build references assets it doesn't ship; failed loads returned NULL `char*` that fed
`std::string(NULL)`, throwing `std::logic_error("basic_string::_S_construct null not valid")`. The
throw unwound `_goEnter()` past its scene-setup store → next frame deref NULL → SIGSEGV (logged as
`status 0`, not a clean segfault). Root-caused with gdb.

- **Fix:** 1-instruction ARM patch in the game's libstdc++-static path so `_S_construct(NULL)` yields
  an empty string instead of throwing. (Offset is game-specific; found via gdb backtrace.)
- **BAKE THE PATCH INTO THE APK** (replace `lib/armeabi*/lib<game>.so` inside the zip), don't just
  drop a patched `.so` next to the binary — see the gdbserver gotcha below.

---

## 5. Screen orientation — the big one (SOLVED for fixed-function GLES1)

**Symptom:** game rendered correctly but only filled the **left ~75%** of a landscape screen.

**Why:** WMW is a **PORTRAIT** game — it calls `glViewport(0,0,768,1024)` every frame and builds a
portrait projection. On our **landscape 1024×768** surface that portrait render sits in the left
768px. (768/1024 ≈ the "70%".)

**Dead ends:**
- **Portrait SDL surface** `SDL_SetVideoMode(768,1024)` → **black**. The TouchPad's PDK framebuffer
  is **hardwired landscape 1024×768**; a non-native portrait GL surface renders (loading screen
  flashes) but the compositor won't present it. `PDL_SetOrientation()` affects system
  notifications/orientation state (no landscape-revert on kill) but does **not** change the FB.
- **`gles_viewport_hack` alone** swaps viewport *dimensions* (768×1024→1024×768) → fills screen but
  **stretches** (dimension swap is not a rotation).

**Solution (true 90° rotation via apkenv's GLES1 fixed-function hooks):**
WMW imports `glMatrixMode/glLoadIdentity/glOrthof` (fixed-function, **no shaders**), so apkenv's
projection-rotation hooks apply. apkenv's rotation model: a game module sets
`module_hacks->current_orientation`; the platform's `get_orientation()` reports the device's natural
orientation; **when they differ**, apkenv rotates viewport / scissor / projection / input.

Config that gives upright, full-screen, unstretched portrait (held camera-up):
```c
// modules/<game>.c, in init:
GLOBAL_M->module_hacks->current_orientation  = ORIENTATION_PORTRAIT; // the game's render orientation
GLOBAL_M->module_hacks->gles_viewport_hack   = 1;  // swap viewport 768x1024 -> full 1024x768
GLOBAL_M->module_hacks->glOrthof_rotation_hack = 1; // rotate the projection 90deg (glRotatef on glOrthof)
// platform/webos.c:
webos_get_orientation() => ORIENTATION_LANDSCAPE;  // device natural; != PORTRAIT triggers the hacks
// keep the NATIVE landscape surface: SDL_SetVideoMode(0,0,0,...)
```
Enum (apkenv.h): `ORIENTATION_LANDSCAPE=0`, `ORIENTATION_PORTRAIT=1`.

**Generalization:** this works because the engine uses fixed-function matrices. For a **GLES2/shader**
game (sets its MVP via `glUniformMatrix4fv`, which apkenv can't rotate), do a **render-to-FBO + rotated
blit**: redirect the engine's `glBindFramebuffer(0)` to an offscreen 768×1024 FBO, then in
`platform->update()` draw a 90°-rotated textured quad to the real 1024×768 FB (GLES1 fixed-function
blit; device has `GL_OES_framebuffer_object`).

---

## 6. Touch — menus/UI SOLVED; active gameplay still OPEN

**Status:** every UI/menu scene (`Screen_MainMenu`, `Screen_LevelSelect`, the pause menu) is fully
touch-interactive — you navigate menus and launch levels. **Active gameplay (`Screen_WaterTest`):
carve-by-drag and the in-game HUD/pause do NOT respond yet.** The two bugs below were required just
to get UI touch working; gameplay is a separate, still-open problem (section 6c).

### Two bugs in series (UI touch)

Touch on the TouchPad through a slim wrapper needs **two** things right, and both are easy to get
wrong. Symptoms when wrong: taps register in SDL and even reach the engine's native `rendererTouch*`
(you'll see the engine's own `touchMoved ERROR - moved finger not found in map!`), but **nothing in
the game UI reacts, anywhere, no matter where you tap.** That combination is misleading — it looks
like "events aren't reaching the game," but the events ARE arriving; they're malformed and/or in the
wrong coordinate *space*.

### Bug 1 — PDL init order (the 3-layer compositor)
`PDL_Init()` **must be called BEFORE `SDL_Init()`**. The TouchPad has a 3-layer display compositor;
init in the wrong order yields malformed touch events (inconsistent `which`/finger ids across
down/move/up → the engine's per-finger map never matches → `finger not found in map`). Also:
```c
PDL_Init(0);
PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES); // default LESSTOUCHES garbles multi-finger
PDL_GesturesEnable(PDL_FALSE);                      // stop the system eating screen-edge touches
SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);
```
Verify: the `finger not found in map` errors disappear once events are well-formed.

### Bug 2 — NORMALIZED coordinates, not pixels (read the Java path = what ACL runs)
**The decisive find.** Decompile the game's Java touch handler (`baksmali classes.dex`); WMW's
`WMWView.copyTouches` does:
```
_xPos[i] = event.getX(i) / getWidth();    // 0..1, NOT pixels
_yPos[i] = event.getY(i) / getHeight();
... rendererTouchBegan(count, _xPos, _yPos, _ids);
```
So the engine's native touch entrypoints expect **normalized 0..1** coordinates. Feeding raw pixels
(e.g. 334) means the engine hit-tests ~334× the screen off to the side → every tap misses everything,
everywhere (looks exactly like "touch is dead"). **Always read the game's Java `onTouchEvent`/helper
to learn the coordinate convention** — don't assume pixels.

### Putting it together (what worked)
- SDL touch arrives as mouse events: type 5=down, 6=up, 4=motion; `event.button.which` /
  `event.motion.which` is the finger index. The digitizer is physically landscape (raw 0..1024 ×
  0..768) however you hold the device.
- Gate MOVE on a finger actually being down (SDL emits motion with no button held); for a single-touch
  menu, forcing a constant finger id (0) also avoids id churn.
- Invert apkenv's 90° render rotation AND normalize, in one step. Corner-tap calibration
  (tap the 4 visible corners, log raw → desired) gave, for our portrait setup on a 1024×768 surface:
  ```c
  float fx = (768.0f - (float)rawY) / 768.0f;   // 0..1
  float fy = (float)rawX / 1024.0f;             // 0..1
  ```
- Result: tapping a menu button starts the level load. **Interactive.**

Method note: when you're convinced "it's not coordinates," check whether it's the coordinate *space*
(normalized vs pixel, density-scaled, origin) before concluding the events aren't being dispatched.
The corner-tap calibration log (`raw → transformed`) plus the Java helper resolves it quickly.

### 6c. OPEN: active gameplay touch (`Screen_WaterTest`)

What we know (from `/tmp/wmw.log`, normal run — no gdb needed):
- The engine names scenes in analytics: `messageRx() - analytics event ScreenEvent
  {"ScreenEnter":"Screen_WaterTest"}`. Menus are `Screen_LevelSelect`/`Screen_MainMenu`.
- In gameplay, taps **reach the engine well-formed** (TAP log fires, **0 finger errors**) and even trigger
  engine analytics (`MiscEvent {"SwampyPressOnLevel":"0",...}` interleaved with taps). So events ARE
  delivered into the gameplay scene — but the **carve (dig) and HUD/pause actions never fire.**
- Strong tell: after a webOS task-switch the app auto-pauses into a UI scene (pause menu) which **is
  tappable**; tapping Resume drops back into the untappable gameplay state. So it's the **active
  gameplay scene's input path**, not event delivery.
- Ruled out: coordinates/space (UI uses the same `rendererTouchBegan` normalized path and works);
  `sRealScreenSize` (was (0,0); mirrored to (768,1024) via base+0x4705bc — no effect).
- Threading context: `libwmw` calls **0 `pthread_create`** but uses `pthread_mutex` +
  `pthread_cond_wait`/`broadcast` — the threads come from Android's framework (UI thread for
  `onTouchEvent`, GL thread for `onDrawFrame`). Gameplay likely drains a touch queue across that
  thread boundary; our single-threaded shim calls both on one thread, which may not satisfy the
  handoff. Inconclusive — needs proof.

Recommended next step (NOT gdb — it kept silently detaching over SSH, and the gdb-launched instance
didn't even receive input: gdbserver session logged 0 taps): **instrument `libwmw` directly.** Patch a
couple of log writes into the touch path (e.g. at `rendererTouchBegan`'s receiver-vector check
`@ file-offset 0x10f51c`, and wherever the active scene dispatches input) using the same in-apk patch
mechanism (bake into the apk), run normally, and compare a `Screen_WaterTest` touch vs a
`Screen_LevelSelect` touch. That tells us definitively: empty receiver list? world-coord camera
mapping sending the carve off-screen? or a UI-thread/GL-thread queue handoff that never drains.

Hard-won method lesson: **when the user states a diagnostic conclusion ("it's not coordinates")
repeatedly, pivot away from that whole hypothesis class immediately.** Several iterations were wasted
re-testing coordinate variants after the user had ruled them out. Trust the operator's observations.

---

## 7. Device / packaging conventions & hard-won gotchas

- **gdbserver does NOT pass env vars to the inferior.** With `APKENV_KEEP_LIBS`/`APKENV_ASSET_ROOT`
  dropped, apkenv **re-extracts the original lib from the apk**, silently overwriting a patched `.so`
  and reverting fixes (cost hours). **Fixes:** (a) bake patches into the apk; (b) make extraction
  **skip-if-exists by default** (don't gate on the env var); (c) give the platform a **built-in
  asset-root default** so it works without env.
- **Storage split:** `/media/internal` (big data partition) holds the **apk + asset root**; `/var`
  (small, ~62 MB, ext3, **exec OK**) holds only the **exec artifacts** (apkenv binary + bionic libs +
  extracted game `.so`). `/media/internal` is **noexec** — binaries must run from `/var`.
  `/media/cryptofs` is fuse (exec OK for scripts, avoid for binaries).
- **Asset repacking, not runtime extraction.** Repack the game webOS-friendly; for assets the engine
  can't find in the apk, redirect failed `/`-rooted `fopen`/`open` under an asset-root dir
  (`/media/internal/<hidden>`), modeled on ACL's hidden `/media/internal/.android` (use a *different*
  dir).
- **Networking/debug:** VM networking was the blocker, not the device (TouchPad has no firewall).
  Used optware OpenSSH on the device (legacy ciphers — pass
  `-o KexAlgorithms=+diffie-hellman-group1-sha1 -o Ciphers=+aes128-cbc,3des-cbc -o
  HostKeyAlgorithms=+ssh-rsa,ssh-dss -o PubkeyAcceptedAlgorithms=+ssh-rsa,ssh-dss -o MACs=+hmac-sha1`).
  gdb: `gdb-multiarch` (host) + gdbserver (device) over an SSH tunnel; use `127.0.0.1` (device
  `/etc/hosts` has only `localhost.local`). `set auto-solib-add off` + empty `sysroot` to avoid slow
  solib downloads; `add-symbol-file lib<game>_orig.so -o <runtime base>` for symbols (the 1-insn patch
  is length-preserving so orig symbols match the patched lib exactly).
- **/dev/fb0 capture is unreliable** for the GL/compositor layer (triple-buffered; black during video
  overlays) — trust the user's eyes for PDK games.

---

## 8. Resource archives: unpack in advance, never stream in-place (bitten TWICE)

**The recurring trap:** an NDK engine packs its game resources into one archive and
reads them out at runtime. Pulling resources through the shim *at runtime is
unstable* — the menu may limp along, but level/scene loads **spin or fail**. The cure
is the same every time: **UNPACK the archive ahead of time** (host-side), push the
unpacked tree to `/media/internal/<asset-root>`, and let the engine's `fopen`/`open`
hit real files (via the §7 asset-root redirect). Do **not** assume the shim can
stream out of the container.

- **WMW (1st time):** in-apk/in-archive asset reads weren't stable → extracted the
  asset tree on the host, pushed it, redirected reads. That's what made it playable.
- **PvZ HD / Marmalade (2nd time):** game data is in `assets/PvZ.dz`, a **`DTRZ`**
  archive (magic `DTRZ`, `u16` file count + null-terminated **filename table** +
  offset/size table + **zlib-DEFLATE** blobs, stream magic `78 da`). `modules/marmalade.c`
  literally carries `// TODO: extract files (implement dzip algorithm)` and never
  unpacks it. Result: menu renders, but "start adventure" leaves the engine **spinning** —
  `strace` shows it looping `open()` on an **empty** `…/.apkenv/<apk>/compiled/` dir
  (open→close→retry), hunting resources that were never extracted. Touch, coords, and
  GL were all fine; the blocker was purely the un-unpacked archive. Same disease, same
  cure: unpack `.dz` → real files.

**Triage rule (mirrored in `android-apk-port-triage`):** for every candidate, find
**where the resources live**. If they're in a custom container (`.dz`/DTRZ, a packed
`.obb`, an in-`.s3e` blob), **budget an unpack step up front** — it is not optional and
it is not a runtime shim concern.

---

## 9. Status checklist (WMW worked example)

| Area | Status |
|---|---|
| bionic/NDK game loads & runs native on webOS (no ACL) | ✅ proven |
| Menu builds without crashing (std::string fix, baked into apk) | ✅ |
| Full-screen, upright, unstretched **portrait** display | ✅ (fixed-function rotation hacks) |
| Touch — menus/UI navigate, launch levels | ✅ (PDL-before-SDL + **normalized** coords) |
| Touch — active gameplay (carve + HUD) | ⛔ OPEN (gameplay input path; §6c) |
| Sound (FMOD `libfmodex` → webOS audio) | ⬜ not started |
| Package as installable `.ipk` | ⬜ not started |

See also: `touchpad-porting.md` (PDK-game patching field guide) and Claude memory notes
`wrapper-spike-progress`, `acl-anatomy`, `android-apk-port-triage`, `templerun2-port-analysis`.
