# CLAUDE.md — Android NDK → webOS game-wrapper workspace

This folder is a workspace for running **Android NDK games** natively on **webOS** (Palm Pre / HP TouchPad) via a **slim apkenv-based wrapper** — **no ACL** (OpenMobile's full Android runtime). The approach: bionic-linker-as-library + fake-JNI + a webOS SDL/PDL backend + a per-game module.

> This is the **Android NDK shim track**, split off from the original `driver` workspace. The sibling **PDK `.ipk`-patching track** (Pre→TouchPad binary patches of native webOS games) now lives at `/home/jonwise/Projects/touchpad-pdk`.

## Start here
- **`PORTING-PLAYBOOK.md`** — the METHOD for the next game: derive the engine→Java host contract
  statically, diff it against the module, instrument, then test one theory at a time. Distilled from
  WMW + PvZ HD (the PvZ "menu freeze" was an unanswered Android dialog, found in one pass after a
  week of brute force). Read first.
- **`android-port-shim.md`** (this folder) — the full field guide. bionic-linker-as-library + fake-JNI + webOS SDL/PDL backend + per-game module. Worked example **Where's My Water?** now boots, renders full-screen upright **portrait**, and is **touch-interactive** (menu buttons navigate). Hard-won wins documented there:
  - two-toolchain cross-build;
  - bake binary patches *into the apk* (gdbserver drops env → re-extracts the original lib);
  - GLES1 fixed-function rotation hacks for a portrait game on the landscape framebuffer;
  - touch = **`PDL_Init` before `SDL_Init`** (+aggression/gestures) **and feed the engine normalized 0..1 coords** (read the game's Java `onTouchEvent`/`copyTouches` via `baksmali` — that's what ACL runs).
  - The webOS MCP `webos://knowledge/pdk` resource (3-layer compositor + PDL touch) was decisive.
- Cross-session methodology also lives in Claude memory: `wrapper-spike-progress` (the live state of the Where's My Water spike) + `android-apk-port-triage` + `acl-anatomy` + `templerun2-port-analysis`.

## What's in this folder
- **`android-candidates/`** — candidate `.apk`s for porting (incl. `PvZ HD v.1.1 ANDROID.apk`, the shipped one): `wheresmywater_1.0.2.apk` (the active spike), `wheresmywater2_1.0.1.apk`, `cut-the-rope_2.3.apk`, `fruitninja_1.8.8.apk`, `bejeweledblitz_1.4.4.apk`, `flappybird_1.0.apk`, `templerun2_1.2.1.apk`.

## Current state
- **Amazing Alex HD (Rovio ka3d) — ported in ONE PASS (2026-08-26)** via the playbook: booted, music,
  playable on the first device launch; `apkenv/packaging/out/com.apkenv.amazingalex_1.0.0_all.ipk`.
  Trail: `plan/AMAZING-ALEX.md`. Module: `apkenv/modules/angrybirds.c` (now in the webOS build).
- **Plants vs. Zombies HD (Marmalade/Airplay) — SHIPPED (2026-08-26):** playable from the launcher
  icon with audio, centered letterbox; `apkenv/packaging/out/com.apkenv.pvzhd_1.0.1_all.ipk`.
  Full trail: `plan/PVZ-HD-menu-freeze.md`. Module: `apkenv/modules/marmalade.c`.

- **Temple Run 2 (Unity 3.5 + Mono) — RENDERS AND PLAYS (2026-08-27):** `com.apkenv.templerun2`
  1.0.0 launches from the icon, the menu responds to taps, and a run renders complete 3D.
  The last two bugs: Unity built its **fixed-function ES1 device** under an ES2 context (a single
  global byte tested at `libunity+0x2d2f74` — patched in `modules/unity.c`), and the ES2 device was
  then drawing through `libGLES_CM` (the ES1 wrappers now forward shared entry points to
  `libGLESv2` — `apkenv_gles1_bind_driver`). Also: **an ES2 context on this device is refused only
  when it is the first context in the process** (`apkenv_egl_warmup()` primes with ES1).
  Full trail: **`plan/TEMPLERUN2-RENDER-INPUT.md`**; history `plan/TEMPLERUN2*.md`.
  Tools: `apkenv/tools/tr2-run.sh` (one-command device loop), `APKENV_GL_SNAPSHOT` (see the frame),
  `APKENV_UNITY_AUTOTAP` (scripted taps). **Open: portrait** (manifest says portrait), swipe
  gameplay, audio verification.

## Where's My Water? (first port)
- **Playable end-to-end with audio**, ships as `com.apkenv.wheresmywater` `.ipk` (launcher icon). Portrait via render-to-FBO; FMOD audio pump. Full writeup: `android-port-shim.md`, `apkenv/BUILD-STATE.md`, `plan/STAGE-*.md`.
- WMW2 (same engine family) reached the level but stalls on multi-threaded GL loading — see `plan/STAGE-5-generalize.md` pt4 (open).

## Conventions
- Temp/scratch work goes in the session scratchpad, **never** this folder.
- Keep candidate `.apk`s pristine; bake any binary patches into a working copy, not the original.
