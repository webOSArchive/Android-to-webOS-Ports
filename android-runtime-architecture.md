# android-runtime-architecture.md — finishing the Android→webOS runtime bridge (no ACL)

A design + delivery plan for turning the apkenv spike into a **faithful, minimal Gingerbread
app-host** on webOS. Companion to `android-port-shim.md` (the field guide / worked example). This
doc is the *why it's stuck and how to finish it* — the field guide is the *how we got here*.

> Status feeding this doc: WMW **boots, renders upright portrait, 0 crashes, menus interactive,
> carve playable via a game-specific force-catch stopgap.** Stuck items: in-game touch (only via
> per-game hack), water-sim orientation (landscape physics under portrait render), audio (not
> started). Baseline reference platform: **AOSP Gingerbread (Android 2.3.x, API 9/10)**.

---

## 1. Diagnosis: why opportunistic poking keeps failing

apkenv today is a **per-game native-entrypoint puppeteer, not an Android runtime.** The module
(`modules/wheresmywater.c`) reaches into the engine and calls its JNI entrypoints
(`rendererInit`, `rendererDrawFrame`, `rendererTouchBegan/Moved/Ended`, `accelerometerChanged`)
**directly, with hand-faked arguments, on one thread.** That was the right move to get to *boot* —
but it skips every Android **contract** the engine's *own internal logic* still assumes is true.

The engine is a closed Gingerbread binary. Its internals (WidgetManager finger-catch, fluid grid,
FMOD audio device) were written against guarantees that AOSP 2.3 makes about **input semantics,
the sensor/display coordinate frame, the audio device, the Activity/View lifecycle, and the
UI-thread/GL-thread split.** When we call entrypoints directly, those guarantees silently don't
hold, and the engine misbehaves *below* the entrypoint in ways no amount of poking at the
entrypoint can fix. That's exactly the shape of all three stuck problems:

| Symptom | The Android contract apkenv doesn't honor |
|---|---|
| Carve ignores touch (only force-catch works) | **Input model.** apkenv uses the pre-2.0 *single-pointer* MotionEvent (only `ACTION_DOWN/UP/MOVE`, no `ACTION_POINTER_DOWN/UP`, no pointer-index-in-action), delivered on one thread with no DOWN→MOVE cadence. The engine's per-finger *catch* logic relies on a finger being **registered on DOWN and then hit-tested on a later event / a later frame** — a lifecycle the puppeteer collapses. |
| Water sim runs landscape under portrait render | **Display + Sensor frame.** We rotate only the *GL output* (per-call hooks). The engine derives its *simulation* frame from `Display` size, `getRotation()`, and the accelerometer coordinate frame — none of which we report self-consistently for "portrait app on a landscape-natural tablet." |
| No sound | **Audio device.** apkenv has a single PCM pull-callback wired to SDL, but WMW's FMOD drives `android.media.AudioTrack` through a Java `org.fmod.FMODAudioDevice` thread that **never runs** (no Dalvik). The contract was never implemented. |
| (cross-cutting) endless per-game env knobs | **Lifecycle + threading.** No faithful `onCreate/onResume/surfaceCreated/regainedTop` sequence; no real UI-thread vs GL-thread split. This is the seam where every `APKENV_WMW_*` hack accretes. |

**The reframe:** stop extending the puppeteer. Promote apkenv to a **thin Android Compatibility
Surface** — implement each Android subsystem the engine actually touches *once, faithfully, from the
AOSP 2.3 spec*, backed by a webOS subsystem. Then per-game modules shrink to "load the libs, wire
the entrypoints," with **zero** per-game behavioral hacks. ACL does this by running all of Android
2.3; we do it by implementing the *five contracts games actually depend on*.

---

## 2. Target architecture

```
   ┌──────────────────────────── per-game module (thin) ───────────────────────────┐
   │  wire engine entrypoints • declare orientation/asset facts • NO behavior hacks │
   └───────────────────────────────────────────────────────────────────────────────┘
            ▲              ▲               ▲               ▲              ▲
   ┌────────┴───┐ ┌────────┴────┐ ┌────────┴─────┐ ┌───────┴─────┐ ┌──────┴──────┐
   │  INPUT     │ │ DISPLAY+    │ │   AUDIO      │ │  LIFECYCLE  │ │  TIME/CLOCK │   ← Android
   │  contract  │ │ SENSOR      │ │  contract    │ │ + THREADING │ │  contract   │     contracts
   │ MotionEvent│ │ contract    │ │ AudioTrack/  │ │ Activity/   │ │ uptimeMillis│   (faithful to
   │ multi-ptr  │ │ getRotation │ │ OpenSL/      │ │ View/GL-    │ │ /nanoTime/  │    AOSP 2.3)
   │ +dispatch  │ │ +accel frame│ │ SoundPool    │ │ thread split│ │ gettimeofday│
   └─────┬──────┘ └──────┬──────┘ └──────┬───────┘ └──────┬──────┘ └──────┬──────┘
         │               │               │                │               │
   ┌─────┴───────────────┴───────────────┴────────────────┴───────────────┴──────┐
   │            webOS backing layer  (PDL + SDL 1.2 + GLES_CM + sensors)          │
   │  PDL_Init→SDL  •  SDL multi-mouse `which`  •  SDL audio cb  •  GLES1 blit     │
   └─────────────────────────────────────────────────────────────────────────────┘
   ┌─────────────────────────────────────────────────────────────────────────────┐
   │      fake-JNI  +  bionic linker-as-library  +  harvested 2.3 libc/libm       │  ← already working
   └─────────────────────────────────────────────────────────────────────────────┘
```

**Design rules:**
1. **Contract-faithful, webOS-backed.** Each subsystem implements the *Android 2.3 behavior the
   engine expects*, then maps to a webOS primitive. The engine must not be able to tell it isn't on
   Android, for the slice it uses.
2. **The engine's internal contract is the spec, not the JNI signature.** Honoring
   `rendererTouchBegan(count,xs,ys,ids)`'s *signature* is necessary but not sufficient — we must
   also honor the *cadence, threading, and event lifecycle* Android would have produced.
3. **Per-game modules carry facts, not behavior.** "This game is portrait," "assets live here,"
   "these are the entrypoints." No `_acceptFinger` vtable pokes, no orientation sign-twiddling.
4. **Instrumentation is a permanent subsystem, not throwaway.** We replace gdb-over-SSH guessing
   with a built-in conformance/trace harness (see Phase 0).

---

## 3. The three stuck problems, mapped to contracts

### 3a. Input — the real model (general fix, not force-catch)

**What Android 2.3 guarantees** (the multi-touch MotionEvent contract; values are load-bearing):
- `action` int = `(pointerIndex << 8) | actionMasked`. `ACTION_MASK=0xff`,
  `ACTION_POINTER_INDEX_MASK=0xff00`, `ACTION_POINTER_INDEX_SHIFT=8`.
  `DOWN=0, UP=1, MOVE=2, CANCEL=3, OUTSIDE=4, POINTER_DOWN=5, POINTER_UP=6`.
- Gesture lifecycle: **first** finger → `ACTION_DOWN`; **subsequent** fingers → `ACTION_POINTER_DOWN`
  (with index); drags → `ACTION_MOVE` (batched samples); a non-last finger up → `ACTION_POINTER_UP`;
  last up → `ACTION_UP`. Pointer **ids are stable** for a finger's whole life.
- Delivery is on the **UI/main thread** via the Looper; **rendering is on a separate GL thread**
  (`GLSurfaceView`). Well-behaved engines **enqueue** touch on the UI thread and **drain** it at the
  top of `onDrawFrame` on the GL thread — so there is a **natural ≥1-frame gap between a finger
  being registered and that finger being processed by the simulation.**

**Why the carve dies (our own RE, now explained by the contract):** WMW's `WidgetManager::touchDown`
for a *fresh* finger id takes the **create** path (alloc `FingerInfo`, return) and **never
hit-tests**. The catch happens only for a finger **already in the map** — i.e. on a *later* event or
the per-frame `update()` enter-detection. Our puppeteer registers and moves the finger inside one
collapsed, single-threaded step, so the "seen last frame → now present/inside this frame" transition
the catcher waits for **never occurs.** Force-catch works precisely because it manually performs the
catch the engine would have done a frame later. That is the tell: **the missing piece is the Android
input *cadence + thread split*, not the coordinates** (coords were always faithful — matches
`WMWView.copyTouches`).

**The fix (general):**
1. A real `AndroidMotionEvent` model in the runtime: multi-pointer arrays, stable ids, the
   action-index encoding above, batched MOVE history.
2. A real **InputDispatcher**: SDL multi-mouse (`event.button.which` 0–4) → a per-pointer state
   machine that emits `DOWN / POINTER_DOWN / MOVE / POINTER_UP / UP` with correct ids.
3. Deliver through the **Activity/View pump on a UI thread**, drained by the engine on the **GL
   thread**, so DOWN and the first MOVE land on **different frames** — reproducing Android's natural
   one-frame gap. (The current `APKENV_WMW_UITHREAD` two-thread experiment was the right idea but it
   still dispatched DOWN+MOVE back-to-back; the gap, not just the thread, is what matters.)
4. **Verification gate:** `acceptNewFingerEntered` fires for our finger with **FORCECATCH=0**, and
   carve + HUD buttons both work. When that holds, delete the force-catch and every input env knob.

> Definitive analysis step before coding (one instrumented run, replaces poking): with the existing
> inline hooks, capture the **exact event/frame sequence** of a *working menu tap* vs a *dead
> gameplay drag* — specifically whether the menu path naturally has a frame between create and
> hit-test that the gameplay path lacks, and whether `WidgetManager::update` enter-detection needs
> the finger present for ≥2 `update()` calls. That single comparison confirms the cadence root cause
> and tells us the exact gap to reproduce, instead of trying input permutations.

### 3b. Orientation — one coherent frame for a portrait game on a landscape-natural tablet

**The core fact:** the TouchPad's **natural orientation is landscape (1024×768)**. There are **three
frames** that coincide on a phone (natural=portrait) but **diverge here**:

| Frame | Reported by | Correct value for a portrait WMW on TouchPad |
|---|---|---|
| App/render | `Configuration.orientation`, `Display.getWidth/Height` | `PORTRAIT (1)`, **768×1024** (w<h) |
| Natural-relative rotation | `Display.getRotation()` | **`ROTATION_90`** (or 270) — **never `ROTATION_0`** |
| Device/natural (sensor) | `SensorEvent.values` (accelerometer) | **landscape frame** (+X along the 1024 edge), **never rotates with the app** |

A correct portrait game **remaps the raw (landscape-frame) accelerometer by `getRotation()`**
(`ROTATION_90 → (screenX,screenY) = (-rawY, rawX)`) so gravity points down the portrait screen.

**Why only the physics is wrong:** we rotate the GL output with fixed-function hooks (independent of
gravity), so render is correct *regardless* of the sensor frame — "render-portrait, physics-landscape"
is the **expected signature of an independently mis-specified gravity/sim frame.**

**The fix (two parts):**
1. **General — implement the orientation frame as one coherent contract.** Report
   `Configuration.orientation=PORTRAIT`, `getWidth/Height=768×1024`, `getRotation=ROTATION_90`, and a
   **real accelerometer** (from webOS) in the **raw landscape frame**; feed gravity either raw (if the
   engine remaps by `getRotation`) or **pre-remapped `(-rawY, rawX)`** with the engine's own rotation
   neutralized. Resolve the 90↔270 sign empirically (fluid must pool at portrait-bottom).
2. **WMW-specific structural — the simulation domain itself is rotated.** Your prior A/B testing
   already **falsified accel-feed as WMW's dominant cause** (toggling orientation, injecting gravity,
   FBO-rotation all left the water unchanged). So for WMW the real bug is the **fluid grid / terrain
   bitmap built from the landscape surface dims** (`_initFluidsWithBounds(AABB)` →
   `Walaber::Grid::Grid(...)` deriving bounds from 1024×768 or a zero `sRealScreenSize` instead of the
   portrait logical 768×1024). Fix the *domain extents to portrait*, with the orientation contract as
   the correctness baseline once the grid is right.
3. **Replace the per-call GLES rotation hooks** with the cleaner model already identified: **render
   the engine into a 768×1024 portrait FBO, then blit one 90°-rotated quad to the 1024×768 screen at
   presentation.** This makes orientation a single deterministic transform instead of N fragile
   per-`glOrthof`/`glViewport`/`glScissor` hooks, and it's the GLES2/shader-game path too.

### 3c. Audio — AudioTrack-over-ring-buffer + a C reimplementation of FMOD's device thread

**The contract:** WMW's FMOD drives `android.media.AudioTrack` (push, **blocking** `write()` in API
9) from a Java `org.fmod.FMODAudioDevice` thread that loops `fmodProcess(ByteBuffer)` →
`AudioTrack.write(...)`. No Dalvik ⇒ that Java thread never runs ⇒ silence.

**The fix (general, and the thinnest faithful point):**
1. Implement **`android/media/AudioTrack`** in fake-JNI: `getMinBufferSize(III)I`, `<init>(IIIIII)V`
   + `(IIIIIII)V`, `play()V`, `write([BII)I`, `write([SII)I`, `pause()V`, `stop()V`, `flush()V`,
   `release()V`, `setStereoVolume(FF)I`, `getState/getPlayState()I`. Decode channelConfig from **both**
   the deprecated (`STEREO=3`) and modern (`CHANNEL_OUT_STEREO=0xC`) constant families.
2. Back it with an **SPSC lock-free ring buffer**; the existing **SDL audio pull-callback drains it**
   (zero-fill on underrun). Blocking `write()` = faithful realtime back-pressure for free.
3. **Reimplement `org.fmod.FMODAudioDevice.run()` in C** as the producer pump: accept
   `RegisterNatives(fmodProcess, fmodGetInfo)`, hand FMOD a direct `ByteBuffer`
   (`NewDirectByteBuffer`), spawn a high-priority pthread that loops `fmodProcess(buf)` →
   `AudioTrack.write(buf)`. Read the mix rate from `fmodGetInfo` (don't assume 44100 — FMOD often
   mixes 24000). S16↔`AUDIO_S16SYS` is a memcpy on ARM.
4. **Verification gate:** menu music + carve SFX audible, no dropouts. (apkenv's `audio/` + `mixer/`
   abstraction already exists and is SDL-wired — this is filling in the JNI class + the pump, not new
   plumbing.)
5. Later/optional for generality: stub **OpenSL ES** (FMOD's autodetect default on 2.3+ — confirm per
   `.so` with `strings libfmodex.so | grep -iE 'OpenSL|AudioTrack|org/fmod'`) draining the same ring,
   and `SoundPool`/`MediaPlayer` with bundled OGG/MP3 decoders feeding the same sink.

---

## 4. Phased delivery plan

Each phase ends with a **verification gate** (an observable behavior, on-device) and a
**de-hack** step (delete the env knobs/stopgaps that phase makes obsolete). Phases 1/2/4 are
intertwined through the lifecycle/threading model, so 4 is pulled early.

**Phase 0 — Conformance & trace harness (permanent).**
Generalize the existing inline-hook facility into a reusable *Android-contract probe*: a trace
channel + a small set of conformance assertions (event lifecycle ordering, orientation-frame
self-consistency, audio underrun count). This is what replaces "poke and squint." *Gate:* one
command emits a labeled trace of input cadence, orientation-frame values, and audio buffer health.

**Phase 1 — Lifecycle + threading core (the seam under everything).**
Faithful `onCreate→surfaceCreated→onResume→regainedTop` sequencing; a real **UI thread + GL thread**
model as the default (not an env knob). *Gate:* task-switch/resume path works without special-casing;
the engine sees a real frame boundary between input delivery and input processing.

**Phase 2 — Input contract.** §3a. *Gate:* carve + HUD work with **FORCECATCH=0**; delete
force-catch + all `APKENV_WMW_{MULTITOUCH,UITHREAD,FORCECATCH,CALLREGAIN,FORCEINPUT,DOUBLEDOWN,
ENTERCATCH,REHIT,REHIT2}` knobs.

**Phase 3 — Display/Sensor/Orientation contract.** §3b. Real accelerometer from webOS; coherent
portrait-on-landscape-natural frame; render-to-portrait-FBO + single rotated blit; WMW fluid-grid
domain fix. *Gate:* tilt changes gravity correctly **and** water settles to portrait-bottom; delete
`APKENV_WMW_{ORIENT,GX,GY,ACCEL}` and the per-call GLES rotation hacks.

**Phase 4 — Audio contract.** §3c. *Gate:* music + SFX audible.

**Phase 5 — Generalize & validate on a 2nd game.** Move every subsystem to default-on, strip the
debug build, and bring up a second candidate (WheresMyWater2 or Cut the Rope) **with no new per-game
behavior hacks** — the proof the runtime generalizes. *Gate:* second game boots + is input/orientation/
audio-correct using only "facts" in its module.

**Phase 6 — Packaging.** `.ipk` via `palm-package` (PalmSDK): exec artifacts under the app dir,
assets/apk staged, `type:"pdk"` appinfo, launcher wrapper. *Gate:* installs + launches from the
TouchPad launcher, no shell scripts.

---

## 5. Risks, open questions, decisions

- **Real accelerometer is deferred** (not blocking Phase 3). Likely source = the **webOS Luna service
  bus** (sensor/orientation service); cheapest first probe = an **SDL joystick-axis mapping**
  (`platform/webos.c` already inits `SDL_INIT_JOYSTICK`); fallback = a kernel `/dev/input` node. Full
  research stub: `plan/accelerometer-luna.md`. Phase 3 is fully verifiable with a synthetic-but-
  correctly-framed vector; only live-tilt gameplay needs the real sensor.
- **One external fact still worth a cheap pull:** the precise Gingerbread guarantee on DOWN→MOVE
  delivery separation across Looper iterations. The architecture is robust either way (faithful
  multi-pointer + thread split + one-frame gap is correct regardless), but confirming it sharpens
  Phase 2's gate. The Phase-0 instrumented menu-vs-gameplay comparison settles it empirically anyway.
- **Scope decision (recommend the middle path):** *(a)* keep polishing WMW with stopgaps — rejected,
  it's what's failing; *(b)* full faithful-runtime rewrite up front — too much before payoff;
  *(c) recommended:* build the **five contracts as proper subsystems but only as deep as a real game
  exercises them**, proven on WMW, then generalized on game #2. This is the plan above.
- **GLES2/shader games** (Temple Run 2, etc.) need the render-to-FBO+blit orientation path (Phase 3)
  and a richer JNI/`native_app_glue` surface; this architecture is forward-compatible with them but
  WMW (GLES1, fixed-function) stays the proving ground.

---

## 6. What changes in the tree (orientation map for implementers)

- `apkenv.h` — promote `ModuleHacks` grab-bag → typed subsystem configs; add full MotionEvent action
  constants (currently only `ACTION_DOWN/UP/MOVE`).
- **new** `input/` — `AndroidMotionEvent` + InputDispatcher state machine (replaces
  `platform/common/input_transform.c`'s per-game 2D flips).
- **new** `display/` (or extend `compat/android_wrappers.c`) — `Display`/`Configuration` contract;
  one orientation transform feeding both render (FBO blit) and the reported frame.
- `accelerometer/` — real webOS sensor backend (today a stub) + the remap idiom.
- `jni/jnienv.c` + **new** `audio` JNI — `android.media.AudioTrack` class + `org.fmod.FMODAudioDevice`
  pump; ring buffer into existing `audio/`+`mixer/`+SDL.
- `platform/webos.c` — own the UI/GL thread split + lifecycle sequencing (today single-thread).
- `modules/wheresmywater.c` — shrink to facts: entrypoints, portrait flag, asset root, fluid-grid
  portrait-dims patch. Delete all `APKENV_WMW_*` behavior knobs as their phases land.

See also: `android-port-shim.md` (worked example / hard-won gotchas), memories `wrapper-spike-progress`,
`acl-anatomy`, `android-apk-port-triage`.
