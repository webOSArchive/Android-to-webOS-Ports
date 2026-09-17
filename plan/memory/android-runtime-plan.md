---
name: android-runtime-plan
description: "The strategic reframe + staged work plan for finishing the Android→webOS runtime (apkenv as a faithful Gingerbread contract-host, not a per-game puppeteer)"
metadata: 
  node_type: memory
  type: project
  originSessionId: f200c8f7-ba57-4262-8910-7f868d839155
---

Strategic pivot for the Android-NDK→webOS bridge (the [[wrapper-spike-progress]] track), decided
2026-06-27. Opportunistic per-game poking at in-game touch, water-sim orientation, and audio had
stalled; root-caused why and replanned.

**The reframe:** apkenv is a **per-game native-entrypoint puppeteer** (calls engine JNI entrypoints
directly, one thread, faked args) — NOT an Android runtime. The three stuck problems are three
**unimplemented AOSP 2.3 contracts**, not three bugs: (1) Input — apkenv uses the pre-2.0
single-pointer MotionEvent + collapses Android's UI-thread→GL-thread one-frame gap, so WMW's
WidgetManager never "catches" the finger (force-catch is a stopgap); (2) Orientation — we rotate only
GL output, but the engine's sim frame derives from Display size/getRotation/accel coordinate frame
(landscape-natural tablet!) which we don't report self-consistently; for WMW specifically the fluid
GRID is built in landscape dims (accel lever proven not to move it); (3) Audio — FMOD drives
android.media.AudioTrack via a Java FMODAudioDevice thread that never runs (no Dalvik).

**The plan:** promote apkenv to a thin faithful Gingerbread app-host implementing 5 subsystem
contracts once, from the AOSP spec, webOS-backed (PDL/SDL/GLES_CM): Input, Display/Sensor/Orientation,
Audio, Lifecycle/Threading, Time. Per-game modules then carry **facts, not behavior** (entrypoints,
portrait flag, asset root) — no surviving APKENV_WMW_* knobs.

**Docs (in the project, git-tracked):** `android-runtime-architecture.md` = strategy/why; `plan/` =
staged work orders (STAGE-0 harness+input-cadence analysis → 1 lifecycle/threads → 2 input → 3
orientation/sensor → 4 audio → 5 generalize/de-hack on a 2nd game → 6 .ipk; `accelerometer-luna.md`
deferred). Each stage doc is self-contained with an AOSP contract spec, on-device gate, de-hack list,
and a Review checklist — written so other AI models can verify our work per stage. User intends to
bring in other models to check work as we go. Accelerometer deferred (Luna bus / SDL joystick axes).
