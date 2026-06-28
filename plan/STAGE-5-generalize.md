# Stage 5 — Generalize, de-hack, prove on a 2nd game

**Objective:** prove the runtime is a **general Android-contract host**, not a WMW special case: make
every subsystem default-on, strip the debug scaffolding, and bring up a **second game** with **no new
per-game behavior hacks** — only facts in its module.
**Architecture ref:** `../android-runtime-architecture.md` §2 (design rules), Phase 5.
**Depends on:** Stages 2, 3, 4.
**Status:** ◐ In progress — 2nd game (WMW2) triaged, reverse-engineered, module written + compiling in the same binary (self-selecting). On-device bring-up pending (see §8).

## 1. The bar (what "general" means here)

- A per-game module contains **only facts**: the engine's JNI entrypoints, an orientation declaration
  (portrait/landscape), the asset root, and any stability patch baked into the apk. **Zero** behavior
  hacks (no vtable pokes, no input/orientation/audio knobs).
- The five contracts (Input, Display/Sensor/Orientation, Audio, Lifecycle/Threading, Time) are
  default-on and game-agnostic.

## 2. Candidate 2nd game (pick one)

Per `android-apk-port-triage` ranking: **WheresMyWater2** (same engine family — fastest validation of
generalization) or **Cut the Rope** (different engine — stronger generalization proof, but 7MB dex /
more Java). Recommend **WMW2 first** (confirms the runtime isn't overfit to WMW's exact addresses),
then a non-Walaber engine as the real generalization test. Avoid Dalvik-heavy / GLES3 / online-DRM
candidates (FlappyBird=AndEngine/Java, etc.).

## 3. Work items

1. **Default-on:** remove every remaining `APKENV_*` behavior gate (keep only `APKENV_TRACE` and
   genuine config like asset root). The subsystems are the default path.
2. **Strip debug:** remove WMW-specific Stage-0 probes from the shipping module (the *facility* stays);
   produce a clean release build (no `[WMW*]` spam, tracing compiled-out or gated off).
3. **2nd-game module = facts only:** list its JNI exports (`readelf -sW lib*.so | grep Java_`),
   declare orientation + asset root, bake any stability patch into the apk. Do **not** add behavior
   hacks; if something doesn't work, fix the **subsystem**.
4. Capture any subsystem gaps the 2nd game reveals (e.g. it uses OpenSL not AudioTrack, or reads a
   MotionEvent field WMW didn't) and fold them back into the general layer.

## 4. Verification gate (on-device, pass/fail)

- [ ] WMW still passes Stages 2/3/4 gates with the **clean, default-on** build (no knobs, no
      force-catch, no debug spam).
- [ ] The 2nd game **boots, is input-correct, orientation-correct, and has audio** using a module that
      contains only facts — verified by reading the module diff (no behavior hacks present).
- [ ] Any new subsystem work added for game #2 is **general** (benefits WMW too), not a #2 special case.

## 5. De-hack / cleanup when green

- Remove all remaining per-game launchers (`play-*.sh` variants) in favor of one launcher per game.
- This stage *is* the de-hack stage; the tree should have no surviving behavior knobs afterward.

## 6. Review checklist (for an external reviewer)

- [ ] Read both game modules. Do they contain **only** facts (entrypoints, orientation, assets,
      stability patch)? Flag any behavior code.
- [ ] Did generalization for game #2 add **game-specific branches** to the runtime? (It shouldn't —
      contract code is engine-agnostic.)
- [ ] Is the release build free of debug tracing cost and `/tmp` log writes?
- [ ] Does WMW's clean build still pass every prior gate (no regression from de-hacking)?

## 7. Risks / open questions

- A non-Walaber engine may exercise contracts WMW never did (richer JNI, `native_app_glue`, OpenSL,
  different sensor idiom). Budget for one or two genuine subsystem extensions — that's the point of the
  stage, and they must be general.

## 8. Work log

### 2026-06-28 — candidate triage + WMW2 reverse-engineering + module (offline; device untouched)

Done autonomously without the device (the WMW1 `.ipk` install was left pristine
for the user's pending test). All static RE; on-device bring-up is the next step.

**Candidate triage (all 7 apks).** native engine + GL + audio + JNI surface:

| apk | native engine | GL | audio | JNI | verdict |
|---|---|---|---|---|---|
| wheresmywater2 | libwalaber (Walaber) | **GLES1** | **FMOD** | 109 static `Bridge.*` | ✅ **picked** |
| bejeweledblitz | libBejBlitz (SexyApp) | GLES2+1 | **OpenSL** | RegisterNatives (hidden) | new sink + dyn-JNI; later |
| fruitninja | libmortargame (Mortar) | GLES2 | FMOD+OpenSL | 51 static + ad-JNI, 5MB dex | GLES2 + ads; later |
| cut-the-rope | libctr-jni | — | — | 7MB **dex** | Dalvik-heavy; poor fit |
| templerun2 | libunity+**libmono** | — | — | C# assemblies (Mono JIT) | hard; needs Mono |
| flappybird | libandengine (11KB stub) | — | — | all **Java/Dalvik** | not NDK; skip |

**Why WMW2:** it is **GLES1 + FMOD** — exactly the subsystems already built — so
porting it *proves they are game-independent* (the Stage-5 thesis), while still
being real RE: Disney replaced WMW1's ~14 `WMWRenderer.*` methods with a new
**"GameLib Bridge"** (109 static exports under `com.disney.GameLib.Bridge.*`).

**What reused verbatim (general code, no new branches):**
- **FMOD AudioTrack pump** (`audio/fmod_pump.c`) — WMW2 ships the same
  `org.fmod.FMODAudioDevice` glue.
- **GLES1 render-to-portrait-FBO** (Stage 3) — same engine, same path.
- **Touch interface** — WMW2's `jniTouch{Began,Ended}(I[F[F[I)` /
  `jniTouchMoved(I[F[F[F[F[I)` are byte-identical to WMW1's Walaber touch model
  (normalized 0..1 coords, count, ids), so the coord transform + array build copy over.

**New per-game module** `apkenv/modules/wheresmywater2.c` (facts: the bridge
entrypoints + init order). Both modules now compile into ONE binary and
**self-select** via `try_init` (each resolves only its own game's symbols) —
demonstrating the multi-game-host shape. Builds clean (GLIBC_2.4).

Bridge map (readelf + baksmali): master init
`WalaberNativeChassis.jniWalaberChassisStartup(String,String,dirPath,int,lang,
country,String,String,String)`; per-bridge `jniBridgeInit()`; renderer
`jniRenderInit(w,h,xdpi,ydpi)` → `jniRenderAreaCreated/Resized` → per-frame
`jniRenderDrawPreDraw()`+`jniRenderDrawFrame()`; `jniTouch*`; `jniAccelerometerChanged`;
lifecycle `jniWalaberChassisAppPause/Resume` + `jniAppLostFocusPleaseShowPauseMenu`.

**On-device bring-up checklist (the `[BRINGUP]` unknowns — needs a device):**
1. **`jniWalaberChassisStartup` 9 args** — exact mapping of the path/id strings +
   the int config (the module passes apk/home/locale guesses; `d.smali` shows the
   shapes but the obfuscated `As`/`At` getters hide exact values).
2. **`jniBridgeInit` order / which bridges are mandatory** — current set is a guess
   (chassis, render, touch, sensor, appfocus, audio, gameflow).
3. **Instance-method `thiz`** — these natives are `private native` (instance), not
   static like WMW1; the engine may read fields/callbacks off the bridge object.
   The module passes the `GLOBAL_M` dummy (worked for WMW1) — verify it suffices.
4. **`std::string(NULL)` boot bug** — `libwalaber` contains the same
   `_S_construct null not valid` throw class as WMW1; if it crashes on boot, find
   + bake the 1-insn patch (as for WMW1).
5. **Orientation** — assumed portrait (FBO on); confirm WMW2 is portrait.
6. **`jniRenderInit` dpi** — passing 132; tune if UI scale is wrong.
7. In-level touch may need the **frame-paced delivery** + thief handling WMW1
   needed (same engine) — add only if the in-level carve is dead, and keep it general.

Next session (with device): bundle the WMW2 apk under `android/`, build the
`.ipk`, install, launch, read `/media/internal/apkenv-*.log`, and walk the
checklist. Do NOT disturb the WMW1 install until its test is done.
