# Stage 3 — Display / Sensor / Orientation contract

**Objective:** make the engine's **render AND simulation** agree on one orientation frame for a
portrait game on a **landscape-natural** tablet, by reporting a self-consistent
Display/Configuration/Sensor frame and replacing the per-call GLES rotation hacks with a single
render-to-FBO + rotated blit. Fix the WMW water-sim landscape bug at its real cause (the sim domain).
**Architecture ref:** `../android-runtime-architecture.md` §3b, Phase 3.
**Depends on:** Stage 0, Stage 1.
**Status:** ✅ **Core DONE (2026-06-27): water renders + simulates correctly via render-to-portrait-FBO.** Remaining (optional): real accelerometer (deferred, `accelerometer-luna.md`); retire the legacy per-call hooks once FBO is default. See §8.

## 1. The Android 2.3 contract (the spec to honor)

The TouchPad's **natural orientation is landscape (1024×768)**. Three frames that coincide on a phone
**diverge** here; report them all consistently:

| Frame | API | Correct value (portrait game, this device) |
|---|---|---|
| App / render | `Configuration.orientation`, `Display.getWidth/Height` | `ORIENTATION_PORTRAIT (1)`, **768×1024** (w<h) |
| Natural-relative rotation | `Display.getRotation()` | **`ROTATION_90`** (=1) or `270` — **never `ROTATION_0`** |
| Device / natural (sensor) | `SensorEvent.values[0..2]` | **landscape frame**: +X along the 1024 edge, +Y along the 768 edge, +Z out of screen; **never rotates with the app** |

- Accelerometer: m/s², value = (acceleration − gravity). Flat face-up at rest → `(0,0,+9.81)`;
  `GRAVITY_EARTH = 9.80665`. The sensor frame is welded to the **natural** orientation and does **not**
  swap when the app is portrait-locked.
- Constants: `ROTATION_0/90/180/270 = 0/1/2/3`; `ORIENTATION_PORTRAIT=1, LANDSCAPE=2`.
- A correct portrait game **remaps the raw accelerometer by `getRotation()`**:
  `ROTATION_90 → (screenX, screenY) = (-rawY, rawX)` (the `ROTATION_270` dual is `(rawY, -rawX)`; the
  90↔270 sign is the device's natural-landscape convention — resolve empirically: fluid must pool at
  portrait-bottom).

## 2. Current state in apkenv (as-built)

- `platform/webos.c`: `webos_get_orientation()` → `ORIENTATION_LANDSCAPE`; surface kept native
  landscape via `SDL_SetVideoMode(0,0,0, SDL_OPENGLES|SDL_FULLSCREEN)`.
- `compat/gles_wrappers.c`: three **per-call** rotation hooks (`my_glOrthof` 90° rotate, `my_glViewport`
  + `my_glScissor` x↔y/w↔h swap), gated by `apkenv_bound_fbo==0`. Fragile; multiple render paths rotate
  inconsistently — **this is why only the water is wrong.**
- Accelerometer = stub (`accelerometer/` interface registered, `sdl_accelerometer` is a no-op). WMW
  feeds a **constant** `(gx,gy)=(0,-9.81)` via `accelerometerChanged` each frame, but the engine's
  guards (`this+0x64/0x14e/0x168`) ignore it unless tilt is enabled → no-op.
- **Crucial prior evidence:** toggling `CurrentDeviceOrientation` (`0x46f97c`), injecting gravity, and
  the FBO-aware rotation fix **all left the water unchanged.** ⇒ for WMW the accelerometer is **not**
  the dominant cause; the **fluid grid is built in the landscape frame.**

## 3. Work items

1. **Implement the Display/Configuration contract** (in `compat/android_wrappers.c` or a new
   `display/`): report `getRotation=ROTATION_90`, `getWidth/Height=768×1024`,
   `Configuration.orientation=PORTRAIT`, density/metrics consistent. One source of truth, consumed by
   both the reported frame and the render transform.
2. **Replace the per-call GLES hooks with render-to-portrait-FBO + single rotated blit:** redirect the
   engine's `glBindFramebuffer(0)` to an offscreen **768×1024** FBO; in `platform->update()` draw one
   90°-rotated textured quad to the real 1024×768 screen (GLES1 fixed-function; device has
   `GL_OES_framebuffer_object`). This is also the GLES2/shader-game path. Remove the
   `glOrthof/glViewport/glScissor` rotation hacks once the blit is correct.
3. **Sensor contract:** implement `SensorManager`/`SensorEvent` for `TYPE_ACCELEROMETER` reporting the
   **raw landscape frame**. Until the real sensor lands (deferred — `accelerometer-luna.md`), feed a
   correctly-*framed* synthetic vector and implement the `getRotation()` remap so the engine path is
   exercised correctly. Decide per-engine whether to feed raw (engine remaps) or pre-remapped
   `(-rawY,rawX)` (engine neutral) — see §1.
4. **WMW structural fix (the actual water bug):** make the fluid grid / terrain-collision bitmap derive
   from the **portrait logical 768×1024**, not the landscape surface (1024×768) or zero `sRealScreenSize`
   (`0x4705bc`). Anchor: `_initFluidsWithBounds(AABB)` → `Walaber::Grid::Grid(...)`; verify the bounds
   source with a Stage-0 hook before patching.

## 4. Verification gate (on-device, pass/fail)

- [ ] Render stays full-screen upright portrait, unstretched (no regression).
- [ ] **Water settles to portrait-bottom** and the fluid sim runs in the **portrait** frame (not the
      90°-rotated landscape ditch).
- [ ] With a tilt fed (synthetic ok for now), gravity direction responds correctly (tilt portrait-left
      → fluid goes left).
- [ ] Trace dump shows the orientation-frame quartet self-consistent (Section 1 table values).

## 5. De-hack / cleanup when green

- Delete `compat/gles_wrappers.c` per-call rotation hooks (replaced by the FBO blit).
- Delete `APKENV_WMW_{ORIENT, GX, GY, ACCEL}` (orientation/gravity now come from the contract).

## 6. Review checklist (for an external reviewer)

- [ ] Are **all four** orientation values (Configuration, width/height, getRotation, sensor frame)
      reported from **one** source and mutually consistent per Section 1?
- [ ] Is render rotation now **one** transform (FBO blit), or still N per-call hooks?
- [ ] Is the water fix the **sim-domain** (grid bounds in portrait), confirmed by a hook on the bounds
      source — or another output-side rotation patch that will whack-a-mole?
- [ ] Does the accelerometer report the **natural (landscape) frame** and leave remapping to
      `getRotation()`, matching Android — or does it pre-bake an app-frame assumption?
- [ ] Is the 90↔270 sign chosen by **observation** (fluid pools bottom), with the reasoning recorded?

## 7. Risks / open questions

- Real accelerometer source is deferred (`accelerometer-luna.md`); Luna sensor bus or an SDL
  joystick-axis mapping are the leading candidates. Stage 3 is fully verifiable with a correctly-framed
  synthetic vector; only live-tilt gameplay needs the real sensor.
- FBO blit must respect the webOS 3-layer compositor + SDL GL ownership (no raw EGL). The existing
  `apkenv_bound_fbo` tracking helps distinguish engine FBOs from the screen.
- If the engine reads screen size from a global the module mirrors (`sScreenSize` `0x4705a4`), ensure
  the portrait logical size reaches it *before* `initializeGame` caches the camera.

## 8. Work log

### 2026-06-27 — render-to-portrait-FBO built; WATER FIXED ✅
Implemented (env-gated `APKENV_WMW_FBO=1`, A/B vs the working hook build):
- `apkenv.h`: `ModuleHacks.{render_to_fbo, fbo_w, fbo_h}` + `apkenv_fbo_present()` decl.
- `compat/gles_wrappers.c`: offscreen 768×1024 portrait FBO (color tex + depth rb);
  `my_glBindFramebufferOES` redirects the engine's fb-0 binds to it;
  `apkenv_fbo_present()` blits it to the 1024×768 native FB as one 90° rotated
  textured quad (rot env-selectable `APKENV_WMW_FBO_ROT`, default 90°).
- `platform/webos.c`: `apkenv_fbo_present()` before `SDL_GL_SwapBuffers`.
- `modules/wheresmywater.c`: under `APKENV_WMW_FBO`, set `render_to_fbo`, 768×1024,
  `current_orientation = LANDSCAPE` (disables the per-call hooks). Launcher
  `/var/apkenv/play-fbo.sh`.

Two bugs found + fixed during bring-up (each a real apkenv-runtime issue):
1. **70%/horizontal-stretch:** the engine sets `glViewport` only at init; the blit
   changed it to screen size, so subsequent frames rendered into the portrait FBO
   through a landscape viewport. Fixed by save/restoring the viewport in present.
2. **Stuck on the attract screen (`Screen_Credits`), never reaching a usable menu:**
   `present()` left GL state dirty every frame, corrupting the attract screen's
   **between-frame async skeleton load** (`_skeletonLoadedCallback` never fired).
   Fixed by save/restoring ALL GL state present() touches (enables, client arrays,
   texture binding, texenv, active/client-active texture, viewport).

**Result (on-device):** full-screen, upright portrait; intro completes; **the fluid
now simulates + renders in the correct portrait frame (settles to portrait-bottom).**
Confirms the handoff's hypothesis #1: the water bug was the per-call rotation hooks
rotating the fluid's render-to-texture pass inconsistently — the self-consistent
single-FBO frame fixes it. **Gate: render upright ✅, water correct ✅.**

Not an FBO regression: **in-level carve/tap still dead** — that's the pre-existing
Stage-0 capture-arbitration issue (level-1 tutorial `Widget_PushButton` steals the
finger). Menus are fully navigable in FBO mode. Tracked separately.

TODO: make FBO the default + delete the legacy per-call rotation hooks once in-level
touch is solved; resolve 90↔270 sign vs a live accelerometer (deferred).
