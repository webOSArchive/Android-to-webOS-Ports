# CLAUDE.md — Android NDK → webOS game-wrapper workspace

This folder is a workspace for running **Android NDK games** natively on **webOS** (Palm Pre / HP TouchPad) via a **slim apkenv-based wrapper** — **no ACL** (OpenMobile's full Android runtime). The approach: bionic-linker-as-library + fake-JNI + a webOS SDL/PDL backend + a per-game module.

> This is the **Android NDK shim track**, split off from the original `driver` workspace. The sibling **PDK `.ipk`-patching track** (Pre→TouchPad binary patches of native webOS games) now lives at `/home/jonwise/Projects/touchpad-pdk`.

## Start here
- **`android-port-shim.md`** (this folder) — the full field guide. bionic-linker-as-library + fake-JNI + webOS SDL/PDL backend + per-game module. Worked example **Where's My Water?** now boots, renders full-screen upright **portrait**, and is **touch-interactive** (menu buttons navigate). Hard-won wins documented there:
  - two-toolchain cross-build;
  - bake binary patches *into the apk* (gdbserver drops env → re-extracts the original lib);
  - GLES1 fixed-function rotation hacks for a portrait game on the landscape framebuffer;
  - touch = **`PDL_Init` before `SDL_Init`** (+aggression/gestures) **and feed the engine normalized 0..1 coords** (read the game's Java `onTouchEvent`/`copyTouches` via `baksmali` — that's what ACL runs).
  - The webOS MCP `webos://knowledge/pdk` resource (3-layer compositor + PDL touch) was decisive.
- Cross-session methodology also lives in Claude memory: `wrapper-spike-progress` (the live state of the Where's My Water spike) + `android-apk-port-triage` + `acl-anatomy` + `templerun2-port-analysis`.

## What's in this folder
- **`android-candidates/`** — candidate `.apk`s for porting: `wheresmywater_1.0.2.apk` (the active spike), `wheresmywater2_1.0.1.apk`, `cut-the-rope_2.3.apk`, `fruitninja_1.8.8.apk`, `bejeweledblitz_1.4.4.apk`, `flappybird_1.0.apk`, `templerun2_1.2.1.apk`.

## Current state (Where's My Water? spike)
- **BOOTS**, renders full-screen upright **portrait**, menus are **touch-interactive** (buttons navigate, levels launch), **0 crashes**.
- **Open item:** active-gameplay touch (`Screen_WaterTest`) — the in-level digging interaction. See `wrapper-spike-progress` memory + `android-port-shim.md` for the full writeup.

## Conventions
- Temp/scratch work goes in the session scratchpad, **never** this folder.
- Keep candidate `.apk`s pristine; bake any binary patches into a working copy, not the original.
