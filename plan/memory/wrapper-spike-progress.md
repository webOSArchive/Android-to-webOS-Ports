---
name: wrapper-spike-progress
description: "Current state of the apkenv NDK→webOS ports (WMW, WMW2, PvZ HD, Amazing Alex) plus the theories that were REFUTED — read the corrections before re-opening any of them"
metadata:
  node_type: memory
  type: project
  originSessionId: 7a813961-5d5f-46b3-b86d-ff6f66990032
  modified: 2026-08-27T22:26:45.528Z
---

Compressed 2026-08-27 from an 11,500-word chronological log. **The full trail — every address,
experiment and dead end — is archived verbatim in the repo at `plan/WMW-PVZ-TRAIL.md`.** Read that
when you need the reasoning; read this for where things stand. Method for the next game:
`PORTING-PLAYBOOK.md`. See also [[templerun2-port-analysis]], [[webos-pdk-launch-jail]],
[[android-apk-port-triage]].

## Where each port stands

- **Where's My Water? — DONE.** Playable end-to-end with audio, portrait, ships as
  `com.apkenv.wheresmywater` .ipk from the launcher icon.
- **Plants vs. Zombies HD — DONE.** Playable with audio, centred letterbox,
  `com.apkenv.pvzhd` 1.0.1. User: "ready to share with the community."
- **Amazing Alex HD — DONE, ported in ONE pass** (2026-08-26) by following the playbook: playable
  with music on the first device launch. `plan/AMAZING-ALEX.md`.
- **Temple Run 2 — DONE, shipped 1.4.0.** Its own memory: [[templerun2-port-analysis]].
- **Aralon — RELEASED 1.0.0** (2026-09-14). `plan/ARALON.md`.
- **Dead Space (EA Mobile, BLAST engine) — RELEASED 1.0.0** (2026-09-15, same session as Fruit
  Ninja). Menus, 3D intro/cutscenes, gameplay aboard the Ishimura, audio with zero underruns,
  3:2 letterbox. Module `apkenv/modules/eablast.c` (named for the engine — `com.ea.blast` is EA
  Mobile's shared Android host, so it should carry to their other ports). Trail
  `plan/DEAD-SPACE.md`. Open: `requiredMemory` is an estimate not a measurement; only the opening
  has been played.
- **Fruit Ninja 1.8.8 (Halfbrick Mortar) — ported in ONE session** (2026-09-15),
  `com.apkenv.fruitninja` 1.0.0. Module `apkenv/modules/mortar.c`; trail `plan/FRUITNINJA.md`.
  **Operator-confirmed**: graphics and sound right, real touch works, and the speed matches the
  original apk on a same-era Android tablet. Every mode reached (Classic/Arcade/Zen/Dojo/Extras/
  Multiplayer setup), results screen, saves. A 38-minute session gave 862 touch events, 2290 s of
  audio with `underrun +0`, and a 59.0 fps median.
  **One open defect: SIGSEGV on pause** (`mortar_pause`, the moment focus is lost). Seen once, not
  reproduced; the dump's PC is untrustworthy. The contract leads are in `plan/FRUITNINJA.md` §8 —
  the Java host stops the GL render thread *before* `native_onPause`, and delivers focus-loss and
  pause as separate callbacks, where the module fires all three natives back-to-back.
- **Where's My Water? 2 — OPEN, precisely diagnosed.** Boots, menu, audio, enters the level, then
  deadlocks: it streams level textures on a **background GL thread**, apkenv has a single GL context
  on main, so the worker parks in `kgsl_yamato_waittimestamp` (Adreno fence) and main waits on it.
  Needs a GL-threading project (marshal worker GL to main / shared context / force a sync load).
  `plan/STAGE-5-generalize.md` §8 pt4.

## Corrections — theories that were REFUTED. Do not re-open without reading these.

**Fruit Ninja: "`native_threadEntry` is inert in this build" was WRONG.** The static pass found
exactly one reference to the string (its own `JNINativeMethod` entry) and no Java caller, and I
wrote it off as leftovers. It is how the engine attaches its audio thread to the VM, and it is bound
by `RegisterNatives` — which only ever runs from the engine's **`JNI_OnLoad`**, which the module was
not calling. One missing call explained no sound, a worker thread that exited instantly, and dialogs
that could never have been answered. See [[static-answer-before-dynamic-probe]]: grep the binary
first, but let the always-on tracer overrule you.

**PvZ HD's menu freeze — the whole 06-29 investigation targeted the WRONG BINARY.** `libpvz.so` is
only the Airplay 4.x *runtime*; the game is `assets/PvZ.s3e` (LZMA-Alone, Sexy framework). Days went
into a "cooperative s3e-fibre stall" model. The actual cause was a **host-contract gap**: PvZ's
first-run "Enter your name" prompt calls Java `AirplayView.getInputString`, then spins
`deviceYield(20)` until `setInputText` answers. The module never answered. The freezing tap's own
log line said `[MARMCALL] getInputString` the whole time. Fixed by answering
(`APKENV_MARM_READSTRING`, default "Player") plus an always-on `[MARM-JNI] UNHANDLED` tracer.
Casualties of that wrong turn, each independently disproven on device: the s3e **free-RAM check**
(patched to always pass — no change); the loader **gate fn `0x1f690`** (forced to return 0 — heap
corruption, it does real work); the **file-task queue** (count was 0, already drained); the
**app-thread suspend mechanism** (live read showed `stat=0`, `count=1` — nothing was parked);
**coop-yield** replication (fired 8000+ times, no effect).

**Two inverted readings inside that same trail**, worth knowing because the names mislead:
`suspendAppThreads` parks **the caller**, not other threads; and `resumeAppThreads` is nearly a
no-op that never signals the cond — so an entire "call resume to wake the worker" approach could
never have worked.

**WMW's carve bug was NOT "the finger is never caught".** It IS caught — by a full-screen
`Widget_PushButton` that sorts ahead of the carve `Widget_FingerCatcher` in the entered-tree order
and swallows every touch. The problem was widget-capture **arbitration**, and the force-catch
stopgap was masking it. Also disproven along the way: coordinates, per-frame dt, touchMoved-catch,
delivery, and a real **UI/GL thread split** (837 dispatches from a dedicated UI thread, still zero
engine response).

**"It's not coordinates" was half right, and the half that was wrong cost the most.** Delivery and
pixel values were fine; the engine wanted **normalized 0..1** coords (`WMWView.copyTouches` divides
by width/height). Pixels hit-tested hundreds of screens away, which presents exactly as "nothing
registers anywhere". *When certain it is not coordinates, check the coordinate SPACE before
concluding dispatch is broken.* See [[trust-operator-diagnosis]].

**Other retired conclusions:** WMW's early crash was **not** file I/O — it was `std::string(NULL)`
throwing inside a statically-linked libstdc++, unwinding past the scene setup (fixed by a
1-instruction patch **baked into the apk**). The "no root element in XML" lead was benign (WMW
*Lite* genuinely lacks those files). WMW2's level-load stall was **not** memory (412 MB free). The
water-orientation bug is fixed — render-to-portrait-FBO made world, UI and fluid agree.

## Live facts still worth having

- **Device:** HP TouchPad, webOS, Linux 2.6.35 armv7l, novacom id `topaz-linux`. **GLES1 is
  `libGLES_CM.so`** (old name). `/media/cryptofs` is fuse — run binaries from `/var` or the app dir.
  SSH `.88` needs legacy-cipher options (`apkenv/tools/deploy-tp.sh`); USB/novacom is more reliable.
- **`gdbserver` does not pass env vars to the inferior**, so apkenv re-extracted the *original* lib
  over a patched one and silently reverted the fix. **Bake binary patches into the apk.**
- **`PDL_Init` before `SDL_Init`** (+ touch aggression, gestures off), or the 3-layer compositor
  produces malformed events and inconsistent finger ids.
- **Never keep the port source only in the scratchpad** — it was lost once and had to be
  reconstructed from upstream apkenv plus these notes. It lives in the repo now.
- Device SDL_mixer will not resample or remix music: open **44100 stereo** and ship music at exactly
  that. FMOD's own mix rate is read dynamically (24000 on this device).
- Packaging and the PDK jail: [[webos-pdk-launch-jail]].
