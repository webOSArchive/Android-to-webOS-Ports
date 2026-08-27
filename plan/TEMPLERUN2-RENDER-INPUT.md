# Temple Run 2 — from "menu renders" to "playable" (plan, 2026-08-27)

Supersedes `plan/TEMPLERUN2-MONO.md` §8–§9 for the three open symptoms. Read this first;
§9 of the Mono plan still holds for everything it says is *verified* (events arrive from the
launcher, `nativeTouch` signature/order, `GetStringUTFChars` copy rule, harness paths).

## 0. What the review found (static evidence, all reproducible offline)

Three of §9's "settled" conclusions are wrong, and the three symptoms share two root causes.

| Symptom | §9 said | What the apk actually says |
|---|---|---|
| Mostly purple, silhouettes only | untextured — chase ETC1+alpha / `glTexEnv` | **Unity's error shader (magenta).** Every `TempleRun2/*` and `MADFINGER/*` shader has **4 `SubProgram "gles"` (= GLSL ES 2.0) passes, 0 fixed-function passes, no `Fallback`**. The things that *do* render (`Unlit/Texture`, `Unlit/Transparent*`, `Mobile/Particles/Additive`, `Projector/Multiply`) are exactly the fixed-function ones. `settings.xml`: **`gles_mode=2`**; manifest: **`uses-gl-es 0x20000`**. We are running Unity's ES1.x device (SDL refused ES2 → fell back) on a game built ES2-only. Textures are not the problem and never were. |
| Launches landscape | "engine calls `setOrientation(0)`=LANDSCAPE, manifest declares none — do NOT do portrait" | Manifest: **`android:screenOrientation=1` (PORTRAIT) on every Unity activity**, `uses-feature android.hardware.screen.portrait`. `setOrientation(int)` just forwards its argument to `setRequestedOrientation`; the engine derives that number from what *we* answer (`getOrientation()`/`getDeviceOrientation()` both hard-coded 0 in `unity.c`). The manifest is the author's intent; the engine's number is our own answer echoed back. |
| Taps ignored | log values, check `eventTime` clock, try `nativeQueueGUIEvent` | `UnityPlayer.dispatchTouchEvent(pointerIndex, rawAction, pointerId, x, y, time, **source**)`: the last int of `nativeTouch(IFFIJI)` is the **`MotionEvent` source** — `0x1002` `SOURCE_TOUCHSCREEN` on a real device (`onTouchEvent` hard-codes it when no `g` handler is installed). `unity.c:789` passes **`0`**. Also: DOWN/UP are only sent for the pointer named in `rawAction`'s index byte; every other pointer gets MOVE. |

How to verify each in 30 s (scratch dir `…/scratchpad/tr2`, apk unzipped + `baksmali`'d):
```
aapt dump badging android-candidates/templerun2_1.2.1.apk | grep -E "uses-gl-es|portrait"
aapt dump xmltree … AndroidManifest.xml | grep screenOrientation        # 0x1 = PORTRAIT
unzip -p …apk assets/bin/Data/settings.xml                              # gles_mode=2
strings assets/bin/Data/*.assets | grep -c 'SubProgram "gles '          # 30, all ES2
awk '/dispatchTouchEvent\(IIIFFJI\)/,/end method/' UnityPlayer.smali     # p8 = source
```

So the order is forced: **no polygons without an ES2 context**; portrait needs an ES2-capable FBO
present, so it comes after ES2; the touch fix is independent and cheap, so it goes first.

## 1. Stage T — touch (independent; do first, one device run)

`apkenv/modules/unity.c`:
1. `unity_input()`: pass `0x1002` as the last arg. Log every call once per event
   (`[UN-TOUCH] id=%d action=%d x=%.0f y=%.0f t=%lld`) — §9 asked for this and nothing yet proves
   the call happens with sane values.
2. Multi-finger correctness (not needed for a menu tap, but free): action for finger *f* is
   DOWN/UP only if *f* is the finger the SDL event is about; the module already receives one
   finger per call, so this is already right — leave it.
3. Keep `un_now_ms()` as `CLOCK_MONOTONIC`-based ms rather than `gettimeofday` (Android's
   `uptimeMillis()` is monotonic; a wall clock is fine unless the engine compares against
   `SystemClock` answers we fake elsewhere — check `unity.c`'s `uptimeMillis`/`nanoTime` handlers
   use the same base). One clock, everywhere.

**Test (packaged .ipk only — novacom launches get no input focus):** tap "Play" on the menu.
Pass = menu responds. Expect it to: the UI is **NGUI** (`UICamera` raycasts from
`Input.touches`/`mousePosition`, both fed by `nativeTouch`), and NGUI needs nothing else.
If it still fails with `[UN-TOUCH]` lines showing correct pixels, *then* §9's remaining list
applies (`nativeEnableTouchpad`, `k`/`u` flags — `u` is set by `windowFocusChanged(true)`, which
we call, `k` is the pause flag). Do not go past one run on this stage before Stage G — the
menu may also be waiting on a Chartboost/promo callback, which only becomes visible once the
scene is drawable.

## 2. Stage G — an ES2 context (the gate for everything visual)

Evidence that it is obtainable, against §7's "do not retry":
- `webos://knowledge/nizovn-packages`: "Direct EGL + GLES2 — Fast — **Works**, touch flicker".
- `webos://knowledge/pdk` Trap 1: SDL's failure is *its* request shape (`CLIENT_VERSION=2` +
  `RENDERABLE_TYPE=ES2_BIT`, `EGL_BAD_ALLOC`); "raw EGL from the same process succeeds on all
  27 configs". The device has `libGLESv2.so`, the apk requires ES2, and Unity 3.5's own
  `t.smali` config chooser exists precisely to negotiate this.
- The compositor-flicker rule is about apps that **own** the surface via EGL. We keep SDL as
  the surface owner and only change the *context* it creates.

### G1 — passive interposition (measure, don't guess)
apkenv already links `-rdynamic -lEGL -lSDL`, so functions defined in the executable win symbol
resolution for libSDL's own `eglChooseConfig`/`eglCreateContext`/`eglGetError` calls (glibc global
scope order). Add `platform/webos_egl_shim.c` exporting those three, forwarding via
`dlsym(RTLD_NEXT, …)`, logging: every attrib list SDL passes, the configs it gets back (dump
`EGL_RENDERABLE_TYPE`, `EGL_SURFACE_TYPE`, `EGL_RED/GREEN/BLUE/ALPHA/DEPTH_SIZE`,
`EGL_CONFIG_ID` for each), the context attribs, and `eglGetError()` after the failing call.
Run with `APKENV_GLES_VERSION=2` from novacom (no input needed). One run answers *which* call
fails and with what config. This is the KB's own method (Tux Racer diff), just done in-process.

### G2 — active fix, smallest change that the G1 log supports. Candidates in likelihood order:
1. `eglChooseConfig` returns a config without `EGL_OPENGL_ES2_BIT` (SDL's chooser ignores
   the bit, takes `configs[0]`) → context creation with `CLIENT_VERSION=2` fails. Fix: in the
   shim, filter the returned list to configs whose `RENDERABLE_TYPE` has the ES2 bit and
   whose surface type is `WINDOW`.
2. SDL asks for `RENDERABLE_TYPE=ES2_BIT` **and** an attribute the ES2 configs don't have
   (e.g. 16-bit depth + 5/6/5 colour, or `EGL_NATIVE_VISUAL_ID`) → `BAD_ALLOC` on the surface
   or context. Fix: drop the offending attrib / pick 8/8/8/8+24 (also removes the RGB565
   banding the touchpad-pdk track documents).
3. Genuine `EGL_BAD_ALLOC` from the driver for this window (unlikely — nizovn's direct EGL
   works): last resort is creating the context ourselves on SDL's `EGLDisplay`/`EGLSurface`
   (obtainable via the same interposition of `eglCreateWindowSurface`) and accepting the
   flicker for now.

**Stop condition:** `webos_init: surface 1024x768, gles_version=2` and
`glGetString(GL_VERSION)` = "OpenGL ES 2.0 …". Save the log as `plan/logs/tr2-es2-1.log`.
While there, log `GL_EXTENSIONS` once: whether the ES2 context exposes
`GL_OES_compressed_ETC1_RGB8_texture` decides G3.3.

### G3 — make apkenv's ES2 path real (all offline, before the next device run)
1. **Hook-table precedence.** `register_hooks_nodup()` keeps the first table registered, which is
   the ES1 one (DT_NEEDED order). Under an ES2 context the 68 shared names must resolve to
   `gles2_wrappers.c` (they forward to `libGLESv2.so`; the ES1 wrappers forward to
   `libGLES_CM.so`, a different driver front-end). Make the winner follow the *actual*
   `gles_version` chosen at `webos_init`, not registration order.
2. **`glGetString`/EGL stubs.** libunity imports only `eglGetProcAddress` (+2 NV timers) and
   otherwise learns the device from `glGetString`. Confirm the ES2 wrapper's `glGetString` is
   passthrough and log `GL_VERSION`/`GL_RENDERER` once. The `[UN]` log should then show
   "Creating OpenGLES2.0 graphics device" (string is in libunity; it is emitted via
   `__android_log_print` — make sure `liblog` output isn't filtered).
3. **ETC1 on ES2.** If G2's extension list lacks ETC1, move the `apkenv_etc1_decode` call from
   `gles_wrappers.c:843` into `my_gles2_glCompressedTexImage2D` (same 10 lines). If present,
   pass through — and keep the ES1 decoder for the other games.
4. **Shader compile logging.** Wrap `glCompileShader`/`glLinkProgram` in the ES2 wrappers to
   print `glGetShaderInfoLog` on failure (`APKENV_GL_PROBE`). Unity's own "GLES20: failed to
   compile" lines go to logcat; Adreno's GLSL compiler is stricter than the ones these
   shaders shipped against, and a silent compile failure looks *exactly* like today's magenta.
5. `module_hacks->prefer_gles_version = 2` is already set in `unity.c`; the fallback in
   `webos_init` stays, but log loudly when it triggers.

**Test:** packaged `.ipk`, expect the menu's 3D backdrop textured and the character visible.
Save `plan/logs/tr2-es2-2.log`. If some materials are still magenta, G3.4's log names the
shader; that is a per-shader GLSL fix (Adreno strictness: precision qualifiers, `gl_FragColor`
swizzles), done by patching the shader text in the ES2 wrapper's `glShaderSource` (the assets
stay pristine).

## 3. Stage P — portrait

Only after G. Two halves:

1. **Tell the engine the truth.** `unity.c`: `getOrientation()` → 1 (`Configuration.ORIENTATION_PORTRAIT`),
   `getDeviceOrientation()` → 0 (`Surface.ROTATION_0` for a portrait-natural device, i.e.
   "the surface is upright"), `nativeInit(768,1024)` and `nativeResize(768,1024,768,1024)`,
   `getScreenDPI` unchanged. Expect `setOrientation(1)` in the log — the engine echoing the
   manifest now. (Fake `Screen.width/height` follow from `nativeResize`.)
2. **ES2 FBO present.** Port `apkenv_fbo_ensure`/`apkenv_fbo_present` (`gles_wrappers.c:469-620`)
   to ES2: `glGenFramebuffers` (core in ES2, no `OES` suffix), colour texture 768×1024 +
   depth renderbuffer (`GL_DEPTH_COMPONENT16`), and the blit is a 4-vertex program
   (trivial vs/fs, a 90° rotation baked into the quad's texcoords). It must save/restore what
   Unity caches: current program, active texture, bound texture, array buffer, vertex-attrib
   0/1 enables, viewport, scissor/blend/depth/cull enables. `glBindFramebuffer(0)` from the
   engine must be redirected to our FBO (same trick the ES1 path uses with `apkenv_bound_fbo`).
   Set `render_to_fbo=1, fbo_w=768, fbo_h=1024` in `unity.c` like `wheresmywater.c:786-788`.
3. **Touch rotation.** Map SDL landscape pixels to portrait: `x' = y`, `y' = 1023 - x`
   (or the mirror; check against which way the blit rotates). Do it in `unity_input()` only when
   `render_to_fbo` is on. Menus are the test: tap the button you see.

**Test:** packaged `.ipk`; upright portrait menu, taps land. `plan/logs/tr2-portrait-1.log`.

## 4. After that (not planned here)
Gameplay: swipe recognition (NGUI is menu-only; the run uses `Input.touches` deltas — MOVE events
must carry the same `pointerId` as the DOWN), accelerometer tilt (`apkenv_accelerometer` is
wired; Unity reads `Input.acceleration` via `nativeSetInputDeviceEnabled`/sensor JNI — check
`[UN-JNI] UNHANDLED` lines), `requiredMemory` in `appinfo.json` (touchpad-pdk: "menus fine,
black when gameplay loads" = memory quota), audio verification, Chartboost/promo no-ops.

## 5. Rules this plan adds to the playbook
- **A handoff's "settled" is a claim, not a fact.** Three of them fell in ten minutes of `aapt`
  and `strings`. Before spending a device run on a theory, check the manifest, `settings.xml`
  and the asset strings — they are the author's intent and cost nothing.
- **Magenta in a Unity game is the error shader, never a texture.** Count `SubProgram "gles"`
  vs `SetTexture [` per shader before touching texture code.
- **Every int in a JNI native signature means something.** `nativeTouch`'s trailing `I` was
  the `MotionEvent` source; we passed 0. Read the Java caller for *each* argument, not just
  the order.
- **Do not fake a device answer and then trust the engine's reaction to it as evidence.**
  `setOrientation(0)` was our own `getOrientation()=0` coming back.

---

# EXECUTION LOG (2026-08-27, session 2) - what is now fixed and what is not

## Stage T - DONE (user-confirmed on device)
`nativeTouch`'s trailing int is the MotionEvent SOURCE; we passed 0, the real host
passes `0x1002` (SOURCE_TOUCHSCREEN, hard-coded in `UnityPlayer.onTouchEvent`).
With that plus a monotonic `eventTime` (`CLOCK_MONOTONIC`, Android's
`uptimeMillis()` base), **the menu responds to taps**. `[UN-TOUCH]` lines log every
DOWN/UP with pixel coords (`plan/logs/tr2-packaged-5.log`: DOWN 538,505 -> MOVE -> UP).
Package 0.1.3.

## Stage G - the ES2 context is REAL now (and the old "never retry ES2" note was wrong)
1. **G1 (passive EGL interposition) paid for itself immediately.** apkenv is linked
   `-rdynamic -lEGL`, so `eglChooseConfig`/`eglCreateContext`/`eglCreateWindowSurface`
   defined in the executable win over libSDL's calls (`platform/webos_egl_shim.c`).
   It showed SDL is handed config id 5 - `[ES1][ES2]`, WINDOW, rgba 8/8/8/8, depth 16,
   caveat EGL_NONE - and `eglCreateContext(CLIENT_VERSION=2)` still returns
   `EGL_BAD_ALLOC`. **Both planned fixes (filter/choose a better config) were dead on
   arrival; only the measurement told us that.**
2. **`tools/egl2test.c` + `tools/egl2-device-run.sh`** (raw EGL / +PDL / +SDL-ES1-current)
   create ES2 contexts on **27/27 configs in every state**. So: not the driver, not the
   jail, not PDL, not SDL owning the display.
3. **Root cause: the FIRST context in the process must be ES1.** Creating ES2 first
   returns BAD_ALLOC on every config; after one ES1 context has been created (and even
   destroyed), ES2 succeeds on all of them. egl2test only ever saw success because it
   tried ES1 before ES2 per config. `apkenv_egl_warmup()` in `platform/webos_egl_shim.c`
   now does exactly that before SDL asks - always on, `APKENV_EGL_WARMUP=0` to disable.
   Result: `webos_init: surface 1024x768, gles_version=2`, no debug flags.
   **This is general apkenv/webOS infrastructure - any ES2 engine benefits.**
4. **Hook-table precedence is now context-driven** (`compat/hooks.c`): the ~68 names
   GLES1 and GLES2 share are re-pointed at the table matching the LIVE context via
   `apkenv_set_active_gles_version()`, called from `webos_init` after the context
   exists (registration happens earlier, so the first choice is a guess).
   Log: `GLES: re-pointed 68 shared symbols at the ES2 wrappers`.
5. ETC1 decode added to the ES2 wrapper too (it is one of the shared names).
6. Shader compile/link failures now report the driver's own message (`[GLSL]`),
   with the offending GLSL cached from `glShaderSource`.

## The frame grab - stop guessing what the screen looks like
`APKENV_GL_SNAPSHOT=<frame>[,<frame>]` makes `webos_update()` `glReadPixels` the back
buffer before the swap and write `/media/internal/apkenv-snap-<n>.ppm` (`platform/webos.c`).
`glReadPixels` is resolved from the library that owns the live context. There is no
screenshot tool for a GL app on webOS 3.0.5, and `/dev/fb0` does not hold the
accelerated layer, so this is the only way to see the render from the workstation.
(webOS's built-in screenshot writes to `/media/internal/screencaptures` and is worth a
try for non-GL cases.) It works: `plan/logs/` + the PNGs show the title screen exactly.

## Device loop, self-serve
`tools/tr2-run.sh [logname]` = kill -> install -> launch -> wait -> pull log.
**`palm-launch` on an already-running app only refocuses it**; the old process keeps
writing the log, so you pull the PREVIOUS build's output while `md5sum` says the new
binary is installed. `killall` via novacom reports "unexpected EOF from server" and does
not always take - `pidof` + `kill -9` does. This cost two confusing runs.

## THE OPEN PROBLEM, now precisely located
**Unity builds its fixed-function ES1 graphics device even though the live context is ES2.**
Measured with one-shot markers in BOTH wrapper tables (`[GLPATH]`): first
`glMatrixMode`, `glTexEnvf`, `glEnableClientState`, and draws through the ES1 wrapper.
`glCreateShader` is **never** called. So the magenta is Unity's error material for the
8 shaders that ship ES2-only subprograms - exactly as predicted - and the fix is to make
Unity build its GLES20 device, not anything about textures.

What has been ruled out by measurement (do not redo):
- **It is not the config, the driver, the jail, PDL or SDL** (see G1-G3 above).
- **It is not `GL_VERSION`.** libunity binds `glGetString` from the ES1 front-end, which
  reports its own static identity `OpenGL ES-CM 1.1` even with an ES2 context current.
  `my_glGetString` now answers from libGLESv2 when the live context is ES2
  (`OpenGL ES 2.0 1566933`, plus the ES2 extension string) - **and Unity still builds ES1.**
- **It is not entry-point probing.** libunity calls `eglGetProcAddress` exactly twice, for
  `eglGetSystemTimeFrequencyNV`/`eglGetSystemTimeNV`. It binds GL statically via DT_NEEDED,
  and those bind fine (no linker errors, no `GLES2 dlsym MISS`).
- **It is not the missing `nativeRecreateGfxState`.** That call (from `onSurfaceCreated`,
  the real host's device-creation point) is now made before `unityAndroidInit`
  (`modules/unity.c`); it runs clean and changes nothing.
- **Java `gles_mode` never reaches native.** `UnityPlayer.init(I,Z)` passes it only to the
  GLSurfaceView subclass (`UnityPlayer$23`/`u`), which uses it for the EGL context version.
  `nativeSetExtras(Bundle)` carries the *intent* extras, not settings.xml.

### Ranked next steps
1. **Find the branch statically.** `libunity` contains both "Creating OpenGLES1.x graphics
   device" (0x6136e4) and "Creating OpenGLES2.0 graphics device" (0x614c70) and prints
   neither (apkenv's liblog wrapper does not filter, so nothing is being swallowed).
   The lib is PIC, so the string addresses never appear as literals - a naive
   4-byte-xref scan finds nothing and a hand-rolled `add rX, pc` filter got it wrong.
   Use a real disassembler: `pip3 install --break-system-packages capstone`
   (`webos://knowledge/system-internals` recommends it for exactly this), find the two
   xrefs, and read the condition above them. That condition names the input we are
   answering wrongly, which is the whole answer.
2. **PlayerSettings, not settings.xml.** Unity's `targetGlesGraphics` lives in
   `assets/bin/Data/mainData`/`globalgamemanagers`, which libunity reads during
   `unityAndroidInit` - a plausible source for the choice. UnityPy (not installed here)
   or a hand parse would confirm what it says.
3. **Force the issue as a test, not a fix:** make the ES1 fixed-function entry points
   fail/absent so the engine cannot build that device, and see whether it takes the ES2
   path and what it then asks for. Diagnostic only.
4. If Unity's ES2 device is unreachable, the fallback is to accept fixed-function and
   patch the 8 ES2-only shaders' materials - much worse, and only if 1-3 dead-end.

## A correction to an earlier "settled" fact
The Adreno 220 **does** advertise `GL_OES_compressed_ETC1_RGB8_texture` - on BOTH the ES1
and ES2 extension strings (logged in full in `plan/logs/tr2-es2-7.log` /
`tr2-es2-8.log`). The previous session concluded it does not and wrote a CPU ETC1
decoder. The decoder is correct and unit-tested, but the premise was wrong: the
"GL_INVALID_ENUM on every texture" it was built to fix was far more likely the
duplicate-GLES-hook bug sending uploads to the wrong front-end. Re-test native ETC1
before assuming the decoder is needed.

---

# SOLVED (2026-08-27, same session) - Temple Run 2 renders and plays

`com.apkenv.templerun2` **1.0.0**. Title screen fully textured; a scripted tap on PLAY starts a
run and gameplay renders complete (track, sky, HUD, tutorial prompt) - `plan/logs/tr2-gameplay.log`,
frames in `plan/logs/` via `APKENV_GL_SNAPSHOT`. Two bugs stood between "ES2 context" and "pixels".

## Bug 1 - Unity built its fixed-function ES1 device under an ES2 context

**The selector, at `libunity+0x2d2f68`:**
```
2d2f68  ldr  r3, [pc, #44]     ; literal 0x003c6e70
2d2f6c  add  r3, pc, r3        ; -> global byte at libunity+0x699de4
2d2f70  ldrb r3, [r3]
2d2f74  cmp  r3, #0
2d2f78  bne  2d2f88            ; != 0 -> bl +0x2c44ac   (ES2 device init)
2d2f7c  bl   2b69b4            ;         (ES1 device init)  <-- what we were getting
```
**How it was found - the method matters more than the offsets.** Grepping for the obvious strings
is a dead end: `libunity` contains both "Creating OpenGLES1.x graphics device" (+0x6136e4) and
"Creating OpenGLES2.0 graphics device" (+0x614c70), prints **neither**, and **neither has a single
code reference** - not as a literal, not via the ARM `ldr`+`add rX, pc` idiom, not via the Thumb
one. They are dead strings in a release build. What worked instead:
1. One-shot markers in **both** wrapper tables showed the engine calling `glMatrixMode`/`glTexEnvf`/
   `glEnableClientState` and never `glCreateShader` - fixed-function device, measured not inferred.
2. A stack scan at the first `glGetString` (glibc cannot unwind bionic-loaded frames, and
   `__builtin_return_address(1)` is useless with `-fomit-frame-pointer`, so scan the stack for words
   pointing into libunity that are preceded by a BL/BLX - the technique apkenv's crash handler uses)
   gave the chain `+0x2b5ed0` (ES1 caps) <- `+0x2b69c4` (ES1 device init) <- `+0x2d2f80`.
3. Disassembling that one frame showed the branch above. Two independent measurements agree: the
   ES2 arm `+0x2c44ac` is the function whose caps code calls `glGetString` at `+0x2beb14`.

**The fix** (`modules/unity.c: un_force_gles2_device`): set the flag to 1 *and* rewrite `bne` -> `b`,
so a later write to the flag cannot undo it. Every patched word is verified against its expected
value first (`ldrb`/`cmp`/`bne`), so a different libunity build is skipped loudly rather than
corrupted, and it only ever runs when the live context really is ES2. `APKENV_UNITY_GLES2=0` disables.

*Trap worth remembering:* `__builtin_return_address(0)` must be taken **in the wrapper**, not in a
helper it calls. Taken inside the helper it reports the wrapper itself - and it silently "worked"
for one call site only because that one got inlined.

## Bug 2 - the ES2 device was drawing through the ES1 driver

With the ES2 device running, `glCreateShader`/`glUseProgram` appeared and the magenta vanished -
but the frame became **a flat clear colour with no geometry at all**. Draws were still landing in
the ES1 wrapper, which forwards to `libGLES_CM`: a *separate front-end with separate state*, which
knows nothing about the ES2 program and vertex attributes that were bound.

Choosing the right wrapper at symbol-resolution time cannot fix this, and that was worth measuring
rather than assuming: `[HOOKRES]` tracing showed the engine's GOT is bound **while the apk's
libraries load, before the platform has created any context** - so at binding time nobody knows
which context we will get. (Re-pointing the hook tables afterwards, which this session also added,
is therefore necessary but not sufficient.)

**The fix** (`compat/gles_wrappers.c: apkenv_gles1_bind_driver`): leave the wrappers where they are
and re-point the *driver* pointers they call through. Once the context exists, the ~68 shared entry
points resolve from `libGLESv2.so`; ES1-only names (`glMatrixMode`, `glTexEnv`, ...) keep falling
back to `libGLES_CM`, which is correct because an ES2 device never calls them.

## Tools this left behind (reusable for the next port)
- `apkenv_egl_warmup()` - ES1-primed EGL warm-up; **an ES2 context on the TouchPad is refused only
  when it is the first context in the process.**
- `APKENV_GL_SNAPSHOT=<frame>[,...]` - `glReadPixels` -> PPM. See the render from the workstation.
- `APKENV_UNITY_AUTOTAP=<frame>:<x>:<y>` - scripted taps through the real input path, for driving a
  menu with nobody at the device.
- `tools/tr2-run.sh` - kill -> install -> launch -> wait -> pull log, in one command.
- `tools/egl2test.c` + `egl2-device-run.sh` - EGL/ES2 capability matrix (raw / +PDL / +SDL).

## Still open
- **Portrait (Stage P) is untouched** and now unblocked: the manifest says portrait on every Unity
  activity, and the ES2 render-to-FBO present described in Stage P above is the remaining work.
- Swipe gameplay (the run needs MOVE deltas with a stable pointer id), audio verification, and the
  `requiredMemory` quota under a full level are unverified.
- The ETC1 correction above still stands: the Adreno advertises `GL_OES_compressed_ETC1_RGB8_texture`
  on both contexts, so the CPU decoder is probably unnecessary now.

---

# Tilt / accelerometer (2026-08-27)

Landscape is **final** - the game adapts to it and looks right; the portrait work in Stage P above
is cancelled, not deferred.

**apkenv's accelerometer was a dead path on webOS.** `platform/common/sdl_accelerometer_impl.h`
opens `SDL_JoystickOpen(0)`, and webOS 3.0.5 has **no joydev in the kernel** - controllers and
sensors are evdev-only, so `SDL_INIT_JOYSTICK` finds nothing and the accelerometer silently
reported a dead-still device forever (`webos://knowledge/game-controllers`, fact #1). Every module
that reads acceleration (marmalade, trg2, and now unity) was affected, so this fix is general.

**The PDK exposes the sensors directly** (`/opt/PalmPDK/include/PDL_Sensors.h`):
`PDL_SensorExists` / `PDL_EnableSensor` / `PDL_PollSensor` with `PDL_AccelerometerEvent {x,y,z}`.
New `platform/common/pdl_accelerometer_impl.h` implements apkenv's `struct Accelerometer` over it;
`webos.c` registers it and keeps the SDL path as a fallback. Verified on device: `|g| = 1.065`, so
**PDL reports g, not m/s2** - scaled by 9.80665 to meet apkenv's Android-frame contract.
Poll drains to the newest queued sample, or tilt lags behind the device.

**Unity's path for acceleration** (`com.unity3d.player.p.onSensorChanged`, type 1 -> `p$1.run()`):
`UnityPlayer.nativeSensor(x, y, z, timestampNanos)`. There is no native polling path - if the host
never calls it, `Input.acceleration` stays (0,0,0) forever, which is exactly what we had.
`modules/unity.c: un_feed_tilt()` now calls it every frame, applying the host's own transform:

    row = (Display.getOrientation() - 1) & 3        // we answer 0 -> row 3
    d[] = { 1,1,0,1,  -1,1,1,0,  -1,-1,0,1,  1,-1,1,0 }
    K   = -1/9.80665                                // m/s2 -> g, negated
    x' = d[row*4+0] * K * v[d[row*4+2]]             // row 3:  x' =  K*v[1]
    y' = d[row*4+1] * K * v[d[row*4+3]]             //         y' = -K*v[0]
    z' =                 K * v[2]                   //         z' =  K*v[2]

Doing the remap here keeps us bit-identical to what the real Java host would have sent for the
orientation we report, instead of inventing a convention.

**SOLVED - the axis convention (confirmed on device):** `row=0, invy=1`, now the module default.
Derived, not guessed: a scripted tilt (neutral / left 3s / neutral / right 3s) logged with
`APKENV_ACCEL_DEBUG=1` showed physical **left roll -> raw.y = +1**, right roll -> raw.y = -1, and
row 3 was sending that to `Input.acceleration.x` with the correct sign - so the axis we had chosen
was right and *the game was not reading it*. What gave it away was the resting frame: row 3 put a
constant **-0.86** on Y, and a constant value on the axis a game steers with is precisely a constant
drift. It also explained why the drift direction flipped between sessions - that Y carried
`+raw.x`, the pitch/gravity axis, which changes sign with how upright the tablet is held. The game
is portrait-built (manifest) while we present landscape, so its steering axis sits a quarter-turn
from the one our reported orientation implies. Row 0 moves the roll onto Y and parks gravity on X;
`invy` fixed the mirror. Two device runs, no rebuild between them.

**Historical note on the axis convention:** Resting propped up it reads
`raw g = (-0.86, +0.03, -0.62)`, which gives `Input.acceleration = (-0.03, -0.86, +0.62)` - a
neutral steering axis and gravity down the screen, i.e. plausible, but only a physical roll
proves which raw axis is the 1024 edge. `APKENV_ACCEL_MAP` permutes/negates axes at runtime
(e.g. `"y,-x,z"`) and `APKENV_ACCEL_DEBUG=1` logs raw + transformed, so calibration is one
launch and an env edit, no rebuild.

*Build-system footgun, hit again:* `build-webos.sh` rebuilds an object only when its **`.c`** is
newer - it does not track headers. Editing a header-only impl and rebuilding silently keeps the
old object; `touch` the `.c`.

**Calibration loop that made this cheap.** `modules/unity.c` reads `row`/`invx`/`invy` from
`/media/internal/apkenv-tilt.conf` as well as from env. The app directory is read-only and the env
file ships inside the package, so without that file every experiment would have been a repackage +
`palm-install`; with it, a mapping change is one `novacom put` and a relaunch. `row` selects among
the same four orientation rows the real Java host supports, so no setting is a bespoke hack - the
chosen one is exactly what Unity would receive on real hardware in that orientation. The file
remains supported as an override; the shipped defaults need it absent.

**Status (superseded below): Temple Run 2 is fully playable** - menu, touch, textured 3D, and tilt steering.
`com.apkenv.templerun2` **1.0.4**.

---

# Portrait (2026-08-27) - DONE, and it was the right call

Landscape "worked", but the settings screen overlapped itself: the UI is authored for the
portrait the manifest declares. Rendering portrait fixed the layout and is what the game
actually is. Confirmed on device: rotated correctly, no overlap, menus right, **and it still
feels smooth** despite the extra blit.

**It fits exactly.** 768x1024 rotated 90 degrees is 1024x768 - the panel's native size - so the
portrait image fills the screen with no letterboxing and no scaling.

**`compat/fbo_es2.c`** is the ES2 twin of the ES1 render-to-FBO path WMW has used since Stage 3.
The old one could not be reused at all: it is fixed-function (matrix stack, `glTexEnv`, client
arrays) and Unity runs on the GLES2 device. The new one is a texture + depth renderbuffer + a
two-attribute shader blit, with the rotation baked into the quad's texcoords.
`apkenv_fbo_present()` dispatches on `apkenv_active_gles_version()`, so both games keep working.

**What makes it invisible to the engine:** `my_gles2_glBindFramebuffer` redirects every bind of
framebuffer 0 to our offscreen target, exactly as `my_glBindFramebufferOES` does on the ES1 path.
The engine believes it owns the screen.

**State save/restore is the whole risk.** The engine sets GL state once and assumes it survives;
a present that leaves the program, texture, attrib arrays or buffer bindings changed corrupts the
NEXT frame in ways that look like a game bug. `apkenv_fbo_es2_present()` saves and restores
program, active texture, 2D binding, array/element buffer bindings, viewport, six enables, and
the full vertex-attrib state (enabled/size/type/normalized/stride/buffer/pointer) for the two
attributes it uses.

**Touch** is rotated into the portrait surface in `modules/unity.c`, derived from the present
quad's texcoords rather than by trial: for the 90-degree case screen-bottom-left samples the
FBO's top-left, giving `u = 1 - sy/sh`, `v = 1 - sx/sw`, with the engine's Y running down while
GL's V runs up.

**Tilt has to rotate too** (the user caught this before it shipped). The sensor frame turns with
the device: holding the tablet for portrait moves the roll from the sensor's Y to its X, so the
steering axis must come from `v[0]` - rows 1 and 3 - and **row 3 + inverted Y** is the one that
steers correctly. It is now coupled to the panel rotation in code: with `APKENV_FBO_ROT=3` the
player holds the tablet the other way up, so the sign flips back automatically. Landscape keeps
its own measured mapping (`row=0, invy=1`), so `APKENV_UNITY_PORTRAIT=0` still behaves as before.

**A trap that cost a confusing screenshot:** after present we leave the offscreen FBO bound for
the next frame, so `glReadPixels` in the frame grab captured the 768-wide portrait target into a
1024-wide buffer - a tiled, skewed mess that looks exactly like a broken renderer. The grab now
binds the window framebuffer first. **`PDL_SetOrientation` is not a shortcut here**: it only
tells the system what orientation you are drawing in (so notifications match), it does not rotate
the surface.

**Shipped: `com.apkenv.templerun2` 1.1.2** - portrait, touch, tilt, textured 3D, swipe. Defaults
verified with the override file deleted. Remaining: **music** (see the audio section above).

---

# Music: what it is NOT (2026-08-27) - still open

Sound effects work; the music track never plays. Six things are ruled out **by measurement**, so
the next person should not re-test them:

1. **Not our audio pump.** An output level meter in `audio/fmod_pump.c` (`APKENV_AUDIO_METER=1`)
   reads FMOD's mixed PCM: pure silence at the menu, while a scripted tap gives `peak=6165`
   decaying over seconds. The pump and the mixer are healthy; music never reaches them.
2. **Not file IO.** The apk opens 278 times with no failures (`APKENV_TRACE_FILES=1`).
3. **Not volume or prefs.** `AudioManager:Awake`'s IL reads "TR Sound Volume" (default 0.75) and
   "TR Music Volume" (default 0.5); our PlayerPrefs store returns 0.5 correctly.
4. **Not the game logic.** `AudioManager:StartGameMusic` **is** called during a run, and its IL is
   `musicSource.Stop(); musicSource.clip = <clip>; musicSource.Play()`. (`StartMainMenuMusic` is
   only ever called from `UIMainMenuViewController:OnHomeButton`, so no menu music on first boot
   is expected, not a bug.)
5. **Not a missing `fmodInitJni`** - though that WAS a real contract gap and is now fixed:
   `org.fmod.FMODAudioDevice.start()` calls it before starting the audio thread and our C pump
   never did. It returns 1. No change to the symptom.
6. **Not a decode error anyone reports.** No Unity/FMOD audio error string, no managed exception.

**What the evidence points at: the stream is opened and then never serviced.**
- The music is uncompressed MP3 inside the apk (`sharedassets0.assets.resS`, `0x1cfd7f8..0x1e35759`).
  A seek-range probe (`APKENV_TRACE_SEEK_RANGE`) shows **17-22 seeks, all early**: two clips'
  headers (one at the start of the .resS, one ~936 KB in), a few KB each, and then **nothing ever
  again** - including while the game believes music is playing.
- **Only 3 threads are ever created in the process, and one of them is our own pump.** FMOD
  normally services streams on a thread of its own; it never asks for one.

So the two candidates worth testing next, in order:
1. **FMOD's async file path** (`fmod_async.cpp` is in libunity, alongside
   `PlatformDependent/AndroidPlayer/FMOD_FileIO.cpp`). If Unity registered async file callbacks,
   stream reads are queued to an FMOD thread that does not exist here - which matches every
   observation: a synchronous header read at open, then silence forever.
2. **FMOD initialised to pump streams from `System::update`** instead of a thread, with the update
   path not reaching the stream. Distinguishing them needs visibility inside libunity's FMOD,
   which is where the cheap instrumentation runs out.

## Tool worth keeping: `apkenv/tools/ildump.py`
Mono's `MONO_VERBOSE_METHOD` dump prints call targets as raw metadata tokens
(`call 0x0600037f`), which is useless until resolved, and there is no monodis/ikdasm here. This
parses ECMA-335 metadata directly: token -> `Class:Method`, list all methods, and (with the
body scanner) **find every caller of a given method**. That is how "who starts the music" was
answered - `GameController:doGameStart/doHandleCountDown/doHandleEndGame` call `StartGameMusic`,
`UIMainMenuViewController:OnHomeButton` calls `StartMainMenuMusic`.

**Method note:** `MONO_VERBOSE_METHOD=<name>` is a cheap "was this managed method ever called"
probe - Mono compiles on first invocation, so no output means never called. Always run a positive
control (`=Awake` prints `converting method UIWidget:Awake ()`); a probe that prints nothing
because it is misconfigured looks exactly like a probe that prints nothing because the method
never ran. And watch for inlining: a small method inlined into its caller never appears.

---

# RELEASE 1.3.0 (2026-08-27) + the last session's lessons

`apkenv/packaging/out/com.apkenv.templerun2_1.3.0_all.ipk` - installed and smoke-tested on device.
Playable: launcher icon, portrait, touch menus, swipe, tilt steering, textured 3D via Unity's own
GLES2 device, sound effects. **Music does not play** (see below); everything else works.
The package ships no debug env - only `APKENV_HOST_MONO`.

## THE find: `nativeInit(II)` is `(glesMode, splashMode)`, not `(width, height)`

Read from `UnityPlayer$24`, the runnable `a(IZ)` queues to the GL thread: its two ints are
`init(IZ)`'s glesMode argument (settings.xml `gles_mode` = 2) and
`getSettings().getInt("splash_mode")` (= 1). **We passed the screen size there for the entire
port.** The engine therefore saw `glesMode = 1024` - not 2, so it built its fixed-function device -
and a splash mode of 768. The real surface size always arrived through `nativeResize`, which we
already called correctly.

Fixing the arguments makes the engine set its own device-select flag:
`[UN-GLES2] device-select flag was 1 - no patch needed`. **The binary patch of
`libunity+0x2d2f78` is now dead code on this build**, kept only as a self-skipping fallback.

That is the real lesson of the whole renderer saga: we spent a session finding and patching the
branch, when the branch was reading a value we were feeding it. **The engine was not choosing
wrong - it was answering the question we actually asked.**

## Two conclusions from earlier in this port that were WRONG

Both were stated confidently in this document and are corrected here:

1. **"FMOD never creates a stream thread."** A `/proc/self/task` sampler shows
   `FMOD stream thr` (idling in `hrtimer_nanosleep`) and `asyncProcessor` (futex wait), plus
   several `UnityMain` threads. apkenv's `pthread_create` log only sees threads created *through
   its own hook*, so counting log lines counted a subset. **Count threads from `/proc`, not from
   your own instrumentation.**
2. **"The music data is read once and never again."** A seek probe cannot see *sequential* reads.
   With per-fd byte accounting the total is **65,536 bytes - exactly one stream buffer** - primed
   and then never consumed. Same shape of error as #1: the probe measured the wrong thing, and the
   conclusion drawn from it was sharper than the evidence.

## Music: the state it is actually in

Not the pump (a meter shows SFX peaks and music silence), not file IO, not volume, not the game
logic (`AudioManager:StartGameMusic` runs and is
`musicSource.Stop(); musicSource.clip = <clip>; musicSource.Play()`), not `fmodInitJni` (a real
contract gap, now closed), not a missing thread. **The stream is created, one 64 KB buffer is
primed, its thread is alive and idle, and the channel never consumes.** Next probe would be the
condition inside the splash/first-frame path (`libunity+0x1cfb00`, reached via `+0x229d2c` and
`+0x275c14`) since splash and music both hang off the engine's first real frame.

## Splash: same neighbourhood

`assets/bin/Data/splash.png` (753 KB, stored uncompressed at `0x17790e4`) **is read** - the stack
scan gives the chain - but nothing is drawn with it: the presents around `unityAndroidInit` issue
**0 draw calls**, then 80 at the first real frame. Forcing extra presents there did not show it
(and made the approach to the menu choppy - reverted).

## Instrumentation left behind, all env-gated

`APKENV_GL_DEBUG` (GL path marks, FBO binds, per-present draw counts), `APKENV_HOOK_DEBUG` (hook
table resolution), `APKENV_TRACE_FILES` (+ mmap), `APKENV_TRACE_SEEK_RANGE=lo-hi` (+ per-fd byte
accounting inside the range), `APKENV_AUDIO_METER`, `APKENV_THREAD_SAMPLE` (the `/proc` sampler),
`APKENV_GL_SNAPSHOT`, `APKENV_UNITY_AUTOTAP`, plus `compat/stackscan.c` and `tools/ildump.py`.

## Honest note on how this session ended

After portrait and tilt landed, **many device cycles produced no user-visible change**. The
`nativeInit` fix was real and retired the binary patch, but the splash and music work after it was
churn: probe, deploy, read, repeat, without a hypothesis sharp enough to falsify in one run. The
user called it, correctly. When a run stops changing what the user can see or hear, stop and say
so rather than deploying again.

---

# Music fallback (2026-08-27) - audible on device, shipped in 1.3.8

The native Unity/FMOD stream remains unresolved. Two non-invasive lifecycle tests ruled out late
AudioTrack startup and the channel's initial paused state. Do not patch or inline-hook libunity's
FMOD internals on the TouchPad: those experiments caused system crashes, and bypassing the
`stream+0x60` worker gate is specifically unsafe.

## What ships

The shipped workaround mixes the game's own music into the known-good FMOD AudioTrack output in
`audio/fmod_pump.c`, after `fmodProcess()` and before the ring-buffer write. It is enabled only by
`APKENV_FMOD_MUSIC_PCM`, so every other game is unchanged. Input is raw little-endian stereo S16 at
FMOD's measured 24000 Hz - the pump does **no** resampling and mixes straight into FMOD's own chunk
buffer, so a file at any other rate or layout plays back at the wrong speed. `APKENV_FMOD_MUSIC_GAIN`
is clamped to 0..1 (Temple Run 2 ships 0.5). The reader loops the file (`pcm_read_loop` rewinds on
EOF, and gives up after one empty rewind so a truncated file cannot spin); the mixer
(`pcm_mix_s16`) is a Q15 add with clipping. Any read or alloc failure just disables the fallback
and leaves FMOD's own output intact.

## Getting the PCM: the asset is two MP3s in a trenchcoat

`assets/bin/Data/sharedassets0.assets.resS` (1277793 bytes, STORED in the apk at
`0x1cfd7f8..0x1e35759`) is two **concatenated** 44.1 kHz stereo MP3s with no container - Unity keeps
only the offsets/lengths, over in `sharedassets0.assets`. Split at `0xe4800`: track 1 is
935936 bytes / 60.08 s (the in-game music, the one we ship) and track 2 is 341857 bytes / 23.47 s.
Decoding the whole `.resS` in one pass silently concatenates both tracks - the split is the
non-obvious step.

`apkenv/tools/tr2-extract-music.sh` does all of it and verifies the result:

```
apkenv/tools/tr2-extract-music.sh [templerun2.apk] [out-dir]
    # default: packaging/templerun2.apk -> packaging/extras/templerun2/
    # -> templerun2-game-24000-s16le.pcm, 5765328 bytes,
    #    md5 988d1789aedd4b84c01b83670eafa959
```

Then package with `EXTRAS=` (build-ipk.sh copies the directory to `android/extras/` in the .ipk):

```
APPID=com.apkenv.templerun2 APK=packaging/templerun2.apk \
APPINFO=packaging/templerun2/appinfo.json ENVFILE=packaging/templerun2/apkenv.env \
HOSTLIBS=hostlibs/webos EXTRAS=packaging/extras/templerun2 ./packaging/build-ipk.sh
```

`packaging/templerun2/apkenv.env` already carries the two settings, with the path relative to the
app directory apkenv `chdir`s into:

```
APKENV_FMOD_MUSIC_PCM=android/extras/templerun2-game-24000-s16le.pcm
APKENV_FMOD_MUSIC_GAIN=0.5
```

**Do not leave the only copy of the PCM in `packaging/stage/`** - `build-ipk.sh` starts with
`rm -rf "$STAGE"`, so the next package of *any* build destroys it. It lives in
`packaging/extras/templerun2/`, which is gitignored along with every other `packaging/extras/`
payload: it is copyrighted game audio, regenerate it from your own apk.

## Device evidence

The output meter (`APKENV_AUDIO_METER=1`) is what separates "engine never started the music" from
"the pump never delivered it": before the fallback it read a flat `peak=0` at the menu.

- `plan/logs/tr2-pcm-fallback-1.log` - 1.3.7, PCM side-loaded to `/media/internal/` with the env
  pointing at the absolute path. `[FMOD-MUSIC] fallback ... bytes=5765328 gain=0.50`, then
  `[FMOD-METER] peak=5523/32767` and up while sitting on the menu. User-confirmed audible.
- `plan/logs/tr2-pcm-fallback-self-contained.log` - 1.3.8, the shipped build: PCM bundled in the
  .ipk, env path relative, `[FMOD-MUSIC] fallback 'android/extras/...' bytes=5765328 gain=0.50`.
  Nothing to side-load.

## The bed obeys the game's own music slider (1.3.9)

The bed is mixed in *underneath* the engine, so nothing in the engine scales it - which at first
meant the in-game music slider did nothing to it. It did not have to stay that way, and the fix
needed no disassembly at all: **a Unity game keeps that setting in PlayerPrefs, and PlayerPrefs is
ours** (`modules/unity.c` - we implement the whole store, persisted to `playerprefs.txt`).

Pulling the live store off the device named the key in one step:

```
$ novacom get file:///media/internal/.apkenv/templerun2.apk/playerprefs.txt
f  TR Sound Volume   0.5
f  TR Music Volume   0.691068411
...
```

(and the game mirrors the same value into its own `gamedata.txt` as `"MusicVolume":0.6910684`,
which is a useful cross-check that this really is the setting and not some internal scalar).

So: `APKENV_FMOD_MUSIC_PREF` names a PlayerPrefs float key. `un_pref_publish_music()` fires on
every `PlayerPrefs.SetFloat` of that key and on load (the store already holds last session's value,
and the game may never call SetFloat again if the player does not touch the slider), and
`apkenv_fmod_music_set_volume()` publishes it to the pump thread as a lone volatile float. The
pump reads it once per chunk and scales `APKENV_FMOD_MUSIC_GAIN` by it - 0 is off.

Two details that matter:

- **Ramp, do not step.** The setting can jump between chunks; a step change in gain at a chunk
  boundary is an audible click. `pcm_mix_s16()` takes a start and end gain and interpolates across
  the chunk (~21 ms - instant to a player, inaudible as a transition).
- **Keep reading the file at zero gain.** The bed then stays in sync with wall-clock time the way a
  real music source does: turning the slider back up resumes where the track would be, not where it
  was muted.

`APKENV_FMOD_MUSIC_GAIN` is now the ceiling *at full setting*, so it went 0.5 -> 0.7: the confirmed
1.3.8 loudness was 0.5 flat, and the player's setting sits at 0.69, so 0.7 x 0.69 reproduces it.
Peak headroom is fine - effects measured ~11000/32767 and the bed ~5500.

Device confirmation on 1.3.9, `plan/logs/tr2-music-volume-1.log`: dragging the in-game slider
streamed **42 live updates** into the pump (`[FMOD-MUSIC] volume setting -> 0.691, 0.658, ... 0.066`
and back up), user-confirmed audible. A true 0.0 was not in that drag, so full-off is reasoned
(`target == 0` skips the mix) rather than observed.

## Known limits

The bed still loops the game's own 60-second track whenever FMOD's mixer is running, rather than
following Unity's per-scene music source: it does not change with the scene, and it does not stop
for the menus that would normally be quiet. What it now does respect is the player's music volume,
including muting. Fixing the native stream properly would replace the whole thing - the env vars
are the only switch.
