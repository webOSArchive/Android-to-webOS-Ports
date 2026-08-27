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
