# Handoff: in-game TOUCH capture + WATER/rotation (WMW on webOS)

Status after a long session. Two open problems, both **precisely localized but not
properly solved**. Goal is a **general** wrapper (many NDK games), so game-specific
hacks are stopgaps, not solutions.

- Source: `/home/jonwise/Projects/webos-android/apkenv/` (build: `./build-webos.sh`).
- Device: HP TouchPad, `root@192.168.10.88` (legacy ssh ciphers — see BUILD-STATE.md).
  Run dir `/var/apkenv/`. Game apk `/media/internal/wheresmywater.apk` (libwmw
  std::string patch baked in, md5 cdf477f1). libwmw is **not stripped** (7569 C++
  symbols) — RE is readable. Extract it from the apk to disassemble.
- Big picture that works: boots, menus interactive, **carve playable (level
  completed, 3 stars) via a STOPGAP**, world/UI render upright.

Addresses below are libwmw **link-time** vaddrs; runtime = +load slide
(slide was 0x2c118000 in recent runs; compute from a known symbol).

================================================================================
## PROBLEM 1 — TOUCH: no widget captures a finger on a fresh "down"
================================================================================

### Verified pipeline (all correct)
finger -> module rendererTouch* (enqueue `ndk::MotionEvent`) -> `rendererDrawFrame`
(0x10deac) calls `pumpEvents()` (0x10d8b0) every frame -> drains queue ->
`ndk::ApplicationContext::touch{Began 0x104674, Moved 0x1045e4, Ended 0x10457c}`
-> `Walaber::ScreenManager::touch{Down,Moved 0x274104,Up}` -> active screen
`WaterConcept::Screen_WaterTest::{touchDown 0x3aea8c, touchMoved 0x3aec0c,
touchUp 0x3aeabc}` -> `Walaber::WidgetManager::{touchDown 0x1edc10,
touchMoved 0x1ea99c, touchUp 0x1ee168}` (the screen's WidgetManager is at
`screen+0x50`).
- Touch gate `ScreenManager::mCurrentTransition` (0x470ec0) is OPEN (nil) in-level.
- Coords are correct: button taps HIT the right button AABBs (valid=1).

### Root cause (proven by inline-hook probe)
`WidgetManager::touchDown`, for a **fresh** finger id, takes the CREATE path (alloc
a `FingerInfo`, return) and **never** reaches the hit-test/capture path (0x1ede08).
That path runs only when the finger id ALREADY EXISTS in the manager's
`std::map<int,FingerInfo*>` (at `wm+0x28`; node: key@16, value(FingerInfo*)@20) with
`FingerInfo+0x14`(captured-widget)==0. We send one BEGAN per finger -> always
fresh -> always CREATE -> **no widget (button OR carver) ever captures on the down.**
Probe result: every in-level touchDown logged `NOT-FOUND -> CREATE`.

- Carve widget = `Walaber::Widget_FingerCatcher` (full-screen, infinite-bounds AABB;
  vtable link 0x469618). Catches via `acceptNewFingerDown` (vtable+0x3c, 0x1fe7bc ->
  unconditional `_acceptFinger` 0x1fe428) — reached ONLY from the touchDown hit-test
  (never happens) — OR `acceptNewFingerEntered` (vtable+0x40, 0x1fe7a4, gated on
  `catcher+0xe0`) — called from `WidgetManager::update(float)` (0x1ece38) per-frame
  enter-detection (calls at 0x1ed1ec/0x1ed6ec).
  **Probe: `acceptNewFingerEntered` is NEVER called for ANY finger ([WMWENTER]=0);
  `_acceptFinger` never fires naturally.**
- `Screen_WaterTest::handleEvent` (0x3b0454): action a=5 (gameplay touch) reads
  `WidgetActionRet+12` = touch count = `catcher+0xbc`; if `<=0` it bails (no carve).
  Count is 0 because the catcher never caught the finger. `World::handleTouch*`
  (0x2c3c84/0x2c3c38/0x2c3bf4) fire 0 times until forced.
- Buttons = `Widget_PushButton` (vtable 0x469920) / `Widget_TimedButton`.
  `acceptNewFingerDown` (0x2095c4) presses (sets pressed state, tracks finger);
  action fires on `releaseFingerUp` (0x209678). The engine's `touchUp` dispatches to
  `FingerInfo+0x14` (captured widget) — so a button only fires if it captured on the
  down. They never do (same fresh-down root). Buttons reachable by id via
  `WidgetManager::getWidget(int)` (0x1ebbac).

### STOPGAP that works (WMW-specific — NOT the real fix)
Each frame in Screen_WaterTest: walk `wm(screen+0x50)` finger-map for id 0; if found
and not yet handled this gesture, call `Widget_FingerCatcher::_acceptFinger(catcher,
0, FingerInfo)` directly (catcher ptr captured from `handleEvent`'s widget arg when
its vtable==FingerCatcher). Env `APKENV_WMW_FORCECATCH`, launcher `play-catch.sh`.
Result: carve works, level completable. **Does NOT fix buttons** (separate widgets).

### Tried and FAILED (touch)
- Real multi-touch aggregation — single-finger swipe is n=1 anyway; no effect.
- UI/GL thread split (`APKENV_WMW_UITHREAD`, separate thread for rendererTouch* vs
  rendererDrawFrame) — no effect.
- Double-BEGAN (dispatch rendererTouchBegan twice) — engine dedups a 2nd down for an
  already-down finger; 2nd touchDown never happens.
- Enter-catch: set `catcher+0x60`(was already 1) and `catcher+0xe0`=1 —
  `acceptNewFingerEntered` STILL never called; no effect.
- REHIT: re-invoke `WidgetManager::touchDown` from our `update()` — captured nothing
  (the engine's hit-test builds its widget list from state only valid inside the full
  ScreenManager->screen->WidgetManager flow; standalone it walks nothing). Also broke
  the carve (regression). Reverted.
- REHIT2: re-invoke `WidgetManager::touchDown` from INSIDE the touchDown hook (real
  flow context) — also captured nothing. Reverted.
- regainedTop() called on entry (enables sub-widgets) — no effect on capture.

### THE UNSOLVED CRUX (general)
On Android the SAME engine code runs, yet the game works — so on Android EITHER the
per-frame enter-detection (`acceptNewFingerEntered`) DOES fire (and ours doesn't —
why? `[WMWENTER]=0`), OR the down hit-test is reached (FingerInfo pre-exists somehow).
**We never determined what state/timing makes the engine's own catch fire on
Android.** That is the key question for a general fix. Suspects not yet ruled out:
a per-frame `dt`/timing dependency in `WidgetManager::update`'s enter-detection
(it takes `float dt`), or a `FingerInfo` field the real framework sets that we don't.
Our delivery is otherwise faithful (matches the game's own `WMWView.copyTouches`
Java, verified via baksmali).

================================================================================
## PROBLEM 2 — WATER/ROTATION: fluid sim renders in a 90° (landscape) frame
================================================================================

### Symptom
World, UI, menus, terrain all render UPRIGHT and correct (they always did). ONLY the
WATER is wrong: the fluid flows/settles in a frame rotated ~90° (landscape) — water
sits "where the landscape ditch would be," not the rendered (portrait) ditch.
Carve digs toward the visual goals correctly; level/goal logic is correct. It is
**only the fluid** that is rotated. (Confirmed OUR bug: Android playthrough is fine.)

### Confirmed INDEPENDENT of (none of these changed the water at all)
- The world camera (world is upright).
- `GameSettings::CurrentDeviceOrientation` (0x46f97c) — set BEFORE rendererInit via
  `APKENV_WMW_ORIENT` (sweeping 0/1 etc.); world stays upright regardless. NOTE this
  global controls the gravity-sign negate in `Screen_WaterTest::accelerometerChanged`
  (negates vertical when ==1) and a per-frame write is too late (camera caches it).
- Accelerometer feed (`APKENV_WMW_ACCEL/GX/GY`). IMPORTANT: `accelerometerChanged`
  (0x3b4fa0) has guards (`[this+0x64]`, `[this+0x14e]`, `[this+0x168]`) that
  early-return unless tilt is enabled — so our feed is likely a NO-OP and base
  gravity is a default constant (we never actually changed gravity). We also never
  fed accel before today at all.
- The FBO-aware rotation fix (below).

### Rotation mechanism in our wrapper
apkenv GLES1 fixed-function hooks: `my_glOrthof` rotates 90° (glRotatef) when
`current_orientation`(PORTRAIT) != platform `get_orientation`(LANDSCAPE); plus
`gles_viewport_hack` (swap viewport) and glScissor swap. Module sets
current_orientation=PORTRAIT + both hacks. Engine uses **glOrthof (31 sites)** and
**glBindFramebufferOES (8 sites = render-to-texture)**, and **no glLoadMatrixf** (all
fixed-function). 
GENERAL FIX MADE THIS SESSION (kept): made the rotation hooks **FBO-aware** —
`apkenv_bound_fbo` (set in my_glBindFramebuffer{,OES}); glOrthof rotate + viewport +
scissor swaps apply ONLY when fb 0 (real screen) is bound, never while rendering into
an offscreen FBO. Correct for render-to-texture games. (Did NOT fix the water.)

### Engine fluid internals (where to look)
- Render: `Walaber::FluidParticleSet::drawParticles` (0x2131b8 etc.) via SpriteBatch
  + camera; vertex build `writeIntoBuffer`/`drawIntoVertBuffer` (0x213860/0x212848).
- Grid/sim: `WaterConcept::Fluids::_initFluidsWithBounds(AABB)` (0x285304) ->
  `Walaber::Grid::Grid(origin, cellW, cellH, cols, rows)`; forces `_calculate_forces`
  (0x284858). Gravity stored by `World::setAccelerometer(Vector2)` (0x2c9300) to
  `fluidObj+12/+16`. Constants `GRAVITY` (0x4710f4), `GRAVITY_MAG` (0x4216b4) —
  couldn't read their float values cleanly; do that.

### Tried and FAILED to move the water
Orientation sweep (ORIENT 0/1), gravity feed + sign (GX/GY both signs and axes),
FBO-aware rotation, accel feed — **water UNCHANGED by all of them.**

### Leading hypotheses (untested)
1. The fluid uses a **render-to-texture (FBO)** whose projection/orientation doesn't
   match the world's rotation, so the composited fluid is 90° off (nested FBO +
   projection transforms — the tangle we couldn't fully trace).
2. The fluid **grid is built with landscape dimensions** — bounds/grid derived from
   the real 1024x768 surface or `sRealScreenSize` (was (0,0) in earlier debugging)
   instead of the portrait logical size (`sScreenSize` was 768x1024).
3. The fluid vertex/particle write swaps axes relative to the world.

### Next steps if continuing
- Read `GRAVITY` (0x4710f4) value; check what dimensions `_initFluidsWithBounds`/
  `Grid::Grid` use (landscape vs portrait); check the fluid's render-texture
  projection vs the world's. A screenshot of flowing water was requested but the
  water mostly flows BEHIND foreground art, hard to capture.

================================================================================
## ARCHITECTURAL TAKEAWAY (for a general, many-games solution)
================================================================================
The per-call display-rotation hooks (rotating individual glOrthof calls) are fragile
with render-to-texture games — different render paths rotate inconsistently (today's
water bug). The robust general approach: tell the game it's **portrait (768x1024)**,
render it to a **portrait FBO**, and **rotate ONCE at final presentation** (rotated
textured-quad blit to the 1024x768 landscape FB). That makes the game's entire
internal frame (sim, gravity, render, UI) self-consistent portrait and decoupled from
the device, and would likely fix water + simplify everything. (Passing portrait dims
to rendererResized crashed earlier — "displaySize must match real surface" — but with
the FBO the game's render target IS 768x1024, so that mismatch should go away.)
Touch still needs the separate general fix (why a fresh-down never captures).

================================================================================
## Device env knobs / launchers (/var/apkenv/)
================================================================================
Launchers: play.sh (baseline), **play-catch.sh** (force-catch carve = current best),
play-mt.sh, play-force.sh, play-regain.sh, play-dd.sh, play-rehit.sh, play-rehit2.sh,
play-orient.sh (`$1=orient $2=gx $3=gy`). stop.sh.
Env vars (modules/wheresmywater.c): APKENV_WMW_{FORCECATCH, MULTITOUCH, UITHREAD,
CALLREGAIN, FORCEINPUT, DOUBLEDOWN, ENTERCATCH, REHIT, REHIT2, ORIENT, ACCEL, GX, GY,
HOOK, SDL_TRACE}. Debug log -> /tmp/wmw.log; hook markers: [WMWTOUCH] [WMWHOOK]
[WMWWMTD] [WMWAABB] [WMWCATCH] [WMWENTER] [WMWEVENT] [WMWFI] [WMWFORCE] [WMWGATE]
[WMWSTACK] [SDLEV] [SDLHB].
