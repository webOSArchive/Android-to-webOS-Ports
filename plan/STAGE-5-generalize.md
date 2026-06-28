# Stage 5 — Generalize, de-hack, prove on a 2nd game

**Objective:** prove the runtime is a **general Android-contract host**, not a WMW special case: make
every subsystem default-on, strip the debug scaffolding, and bring up a **second game** with **no new
per-game behavior hacks** — only facts in its module.
**Architecture ref:** `../android-runtime-architecture.md` §2 (design rules), Phase 5.
**Depends on:** Stages 2, 3, 4.
**Status:** ☐ Not started

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
