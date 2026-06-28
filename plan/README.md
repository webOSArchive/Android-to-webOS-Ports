# Android→webOS runtime — staged work plan

This directory is the **staged execution plan** for finishing the Android-NDK→webOS runtime bridge
(no ACL). The strategy and rationale live in `../android-runtime-architecture.md`; **read that first.**
These stage docs are the *work orders*.

## The one-paragraph reframe (so a cold reader has context)

apkenv today is a **per-game native-entrypoint puppeteer**: it calls a game engine's JNI entrypoints
directly, on one thread, with hand-faked arguments. That booted Where's My Water (WMW) but skips the
Android 2.3 **contracts** the engine's *internal* logic still assumes. The three stuck problems
(in-game touch, water-sim orientation, no audio) are three unimplemented contracts, not three bugs.
The plan promotes apkenv into a **thin, faithful Gingerbread (AOSP 2.3 / API 9-10) app-host** by
implementing the five subsystem contracts games actually use — **Input, Display/Sensor/Orientation,
Audio, Lifecycle/Threading, Time** — once, from the AOSP spec, each backed by a webOS primitive
(PDL + SDL 1.2 + GLES_CM). Per-game modules then carry **facts, not behavior**.

## How to use these docs

**Implementers:** work stages in order (dependencies noted). A stage is *done* only when its
**Verification gate** passes on-device, and you've completed its **De-hack** cleanup. Record evidence
in the stage's Work log.

**External reviewers (other AI models / humans checking our work):** each stage doc is written to be
**self-contained**. Section 1 ("the contract") states the spec with concrete, independently-checkable
values (AOSP constants, JNI signatures, engine addresses). Section 6 ("Review checklist") is the set
of questions you should be able to answer **yes** to — independently verifying faithfulness to the
Android contract, not just "does it run." Flag any place our implementation diverges from Section 1.

## Stages & status

| Stage | Title | Depends on | Status |
|---|---|---|---|
| [0](STAGE-0-harness.md) | Conformance & trace harness (+ the input-cadence analysis run) | — | 🔬 Analysis DONE — root cause found (capture-arbitration, not delivery); harness pending |
| [1](STAGE-1-lifecycle-threading.md) | Lifecycle + UI/GL thread split | 0 | ◐ Partial — frame-paced touch delivery done (the cadence fix); full Activity lifecycle not built |
| [2](STAGE-2-input.md) | Input contract (faithful multi-pointer MotionEvent) | 0, 1 | ✅ In-level touch WORKS — cadence fixed (paced) + carve/HUD blocker (full-screen thief) neutralized; precise-thief refinement pending |
| [3](STAGE-3-orientation-sensor.md) | Display / Sensor / Orientation contract | 0, 1 | ✅ Water FIXED (render-to-portrait-FBO); real accelerometer deferred |
| [4](STAGE-4-audio.md) | Audio contract (AudioTrack + FMOD pump) | 0, 1 | ✅ DONE on device — FMOD AudioTrack pump in C → lock-free ring → SDL; audible, rate=24000 |
| [5](STAGE-5-generalize.md) | Generalize, de-hack, prove on a 2nd game | 2, 3, 4 | ☐ Not started |
| [6](STAGE-6-packaging.md) | `.ipk` packaging | 5 | ✅ DONE on device — self-contained `.ipk`, launches from the launcher (jail path decoded). Future: external/BYO-apk distribution (planned, §8) |
| [accel](accelerometer-luna.md) | (deferred) Real accelerometer via Luna bus / SDL joystick | 3 | ☐ Research |

Stages **1, 2, 3, 4** are independent of each other once **0 and 1** land — they can be parallelized
across people/models. **1 is the seam under 2/3/4** (the real thread split and lifecycle), so it goes
first after the harness.

## Global conventions

- **Contract-faithful, webOS-backed.** Implement the *Android 2.3 behavior the engine expects*, then
  map to a webOS primitive. The engine must not be able to tell it isn't on Android, for the slice it
  uses. "It runs" is not the bar; "it matches the AOSP contract in Section 1" is.
- **Facts, not behavior, in per-game modules.** No `_acceptFinger` vtable pokes, no orientation
  sign-twiddling, no per-game env knobs surviving past their stage. If a module needs a behavior hack
  to work, the corresponding *subsystem* is wrong — fix it there.
- **Every gate is observed on-device.** `/dev/fb0` capture is unreliable for the GL layer; gates are
  user-observable behavior + the trace harness (Stage 0), not screenshots.
- **Analyze, then build.** Where a stage has an open root-cause question, the *first* work item is a
  Stage-0 instrumented run that settles it — not a code permutation.

## Source & device facts (shared anchors)

- Source of truth: `/home/jonwise/Projects/webos-android/apkenv/` (git-tracked). Build:
  `apkenv/build-webos.sh` (two-toolchain: gcc-13 compile against PalmPDK glibc-2.4 headers → PalmPDK
  gcc-4.3.3 link). Full recipe + as-built notes: `apkenv/BUILD-STATE.md`,
  `apkenv/HANDOFF-touch-water.md`.
- Device: HP TouchPad, webOS 3.0.5, Linux 2.6.35, Adreno 220 (**GLES2 max**), native **landscape
  1024×768**, GLES1 lib is the old name `libGLES_CM.so`. SSH/legacy-cipher + run-from-`/var` notes in
  `../android-port-shim.md` §7.
- WMW engine (`libwmw.so`, **not stripped**, 7569 syms) verification anchors used across stages:
  - Touch: `Screen_WaterTest::touch{Down,Moved,Up}` @ `0x3aea8c / 0x3aec0c / 0x3aeabc`;
    `WidgetManager::touchDown` @ `0x1edc10` (hit-test branch `0x1ede08`); finger-map = `std::map<int,
    FingerInfo*>` @ `wm+0x28` (node key@16, value@20); `FingerInfo+20` = caught-widget ptr;
    `Widget_FingerCatcher::_acceptFinger` @ `0x1fe428`, `acceptNewFingerEntered` @ `0x1fe7a4`,
    catch-count @ `catcher+0xbc`, enter-gate @ `catcher+0xe0`.
  - Orientation: `sScreenSize` @ `0x4705a4`, `sRealScreenSize` @ `0x4705bc`,
    `GameSettings::CurrentDeviceOrientation` @ `0x46f97c`; fluid grid via
    `WaterConcept::Fluids::_initFluidsWithBounds(AABB)` → `Walaber::Grid::Grid(origin,cw,ch,cols,rows)`;
    fluid render `Walaber::FluidParticleSet::drawParticles`.
  - Audio: `libfmodex.so`, Java glue `org.fmod.FMODAudioDevice`, natives
    `fmodProcess(Ljava/nio/ByteBuffer;)I` + `fmodGetInfo(I)I`.
  - Stability patch (already baked into the apk): 1-insn `std::string(NULL)` fix @ file-offset
    `0x3f891c`.
