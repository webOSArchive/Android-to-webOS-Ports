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
- **Fruit Ninja 1.8.8 (Halfbrick Mortar) — ported in ONE SESSION (2026-09-15) and confirmed on the
  panel.** `apkenv/packaging/out/com.apkenv.fruitninja_1.0.0_all.ipk` (49 MB, self-contained: the
  apk carries all 87 MB of assets, no OBB). Landscape 1024×768 on the engine's own ES2 device at a
  vsync-locked **59.2 fps**; every mode reached and running (Classic, Arcade, Zen, Dojo, Extras,
  Multiplayer setup), the pause HUD and the end-of-game results screen; audio at 44100/stereo with
  zero underruns; saves and high scores persisting across a reinstall. The user compared it against
  the original apk on an HP 10 G2 Tablet (MT8127 / Mali-450 / Android 5.0.1) and rated the speed
  equal, graphics and sound right. New module:
  `apkenv/modules/mortar.c` (Mortar 1.8.x host generation — NOT the 1.7.x one upstream
  `modules/fruitninja.c` targets). Trail: `plan/FRUITNINJA.md`; contract table
  `plan/fruitninja-contract.txt` (regen: `apkenv/tools/fn-contract.sh`).
  **The lesson worth carrying: call the engine's `JNI_OnLoad`** — it is where an engine
  `RegisterNatives` its own callbacks, and skipping it cost all the game's audio via a worker
  thread that exited the instant it called an unbound `native_threadEntry`. New tools:
  `apkenv/tools/push-run.sh` (binary-only device cycle, no re-install),
  `apkenv/tools/fn-contract.sh`, and `APKENV_MORTAR_AUTOTAP` — synthetic taps and swipes through
  the real input path, which is how the port was driven to a scoring Classic game with nobody
  holding the device.
- **Aralon: Sword and Shadow HD (Unity 4.0.1 + Mono) — RELEASED 1.0.0 (2026-09-14)**, installed
  fresh on the TouchPad and verified (clean env, fresh profile, no debug tracers):
  `apkenv/packaging/out/com.apkenv.aralon_1.0.0_all.ipk` (291 MB, OBB bundled). Landscape, menus +
  touch, New Game → world, SFX, dialogue, ambience and **music**. Three systemic host fixes, all
  gated so Temple Run 2 is untouched: Unity 4's boot order (pre-init `nativeResize` + `nativeRender`),
  an AndroidJavaObject bridge (ReflectionHelper → DisplayMetrics, or the GUI lays out at 0x0), and
  `APKENV_PTHREAD_STACK_CLAMP` — FMOD asks for 8 KB thread stacks, glibc rejects them, and that
  silently killed every MP3 track. Outdoor lighting is slightly less warm than on a Mali tablet —
  parked by the user. Trail: `plan/ARALON.md`; new tools: `APKENV_UNITY_ICALL_TRACE`,
  `APKENV_GL_UPLOADCHECK`, and the Android reference-device method in `PORTING-PLAYBOOK.md`.
- **Screenshots of a running game: `apkenv/tools/grab.sh [<name>]`.** The on-device screenshot
  (and `/dev/fb0`) misses the GL layer, so the game reads its own frame back: the script drops
  `/media/internal/.apkenv/grab` over novacom, the render loop consumes it within ~0.5 s and writes
  the next frame, and the script saves a PNG under `apkenv/packaging/out/screenshots/` (gitignored).
  Always on in binaries built since 2026-09-14; shipped packages older than that lack it.
- **Amazing Alex HD (Rovio ka3d) — ported in ONE PASS (2026-08-26)** via the playbook: booted, music,
  playable on the first device launch; `apkenv/packaging/out/com.apkenv.amazingalex_1.0.0_all.ipk`.
  Trail: `plan/AMAZING-ALEX.md`. Module: `apkenv/modules/angrybirds.c` (now in the webOS build).
- **Plants vs. Zombies HD (Marmalade/Airplay) — SHIPPED (2026-08-26):** playable from the launcher
  icon with audio, centered letterbox; `apkenv/packaging/out/com.apkenv.pvzhd_1.0.1_all.ipk`.
  Full trail: `plan/PVZ-HD-menu-freeze.md`. Module: `apkenv/modules/marmalade.c`.

- **Temple Run 2 (Unity 3.5 + Mono) — RELEASED 1.4.0 (2026-08-27), music + splash working.**
  `apkenv/packaging/out/com.apkenv.templerun2_1.4.0_all.ipk`: launcher icon, **portrait** (ES2
  render-to-FBO rotated present, `apkenv/compat/fbo_es2.c`), touch menus, swipe, tilt steering
  (PDL sensors — apkenv's SDL-joystick accelerometer never worked on webOS), textured 3D on
  Unity's own GLES2 device, sound effects, and music. Unity's native FMOD stream still stalls after
  priming; the safe workaround decodes the game's 60-second MP3 to 24000 Hz stereo S16 at package
  time and mixes it into the existing FMOD AudioTrack pump. The PCM is bundled under
  `android/extras/`, and the bed **obeys the in-game music slider** — the setting lives in
  PlayerPrefs, which apkenv implements, so `APKENV_FMOD_MUSIC_PREF=TR Music Volume` drives the mix
  gain live (0 = off). The **boot splash** is drawn by the host, not the engine: the real libunity
  never opens `splash.png` (the one in `lib/` is a 43 KB proxy — the engine is in `assets/libs/`),
  so `APKENV_SPLASH_RGB` rides the existing rotated present quad and retires on the engine's first
  draw. Full evidence and packaging instructions: `plan/TEMPLERUN2-RENDER-INPUT.md`.
  **The lesson worth carrying:** `nativeInit(II)` is `(glesMode, splashMode)`, NOT
  `(width, height)` — passing the size made the engine pick its fixed-function renderer and skip
  the splash, and cost a session of disassembly. See `PORTING-PLAYBOOK.md`.
  Regenerate the `EXTRAS` payloads with `apkenv/tools/tr2-extract-music.sh` (the `.resS` is **two
  concatenated MP3s**; it splits at `0xe4800` and md5-checks the result) and
  `apkenv/tools/tr2-extract-splash.sh` (raw RGB24, **bottom row first** for GL's origin), then
  package with `EXTRAS=packaging/extras/templerun2`. That dir is gitignored and is the only
  surviving copy — `build-ipk.sh` wipes `packaging/stage/` on every run.
  Tools: `apkenv/tools/tr2-run.sh`, `tr2-extract-music.sh`, `tr2-extract-splash.sh`,
  `tools/ildump.py`, `APKENV_GL_SNAPSHOT`, `APKENV_THREAD_SAMPLE`, `APKENV_TRACE_SEEK_RANGE`,
  `APKENV_AUDIO_METER`, `APKENV_FMOD_MUSIC_PCM`/`_GAIN`/`_PREF`, `APKENV_SPLASH_RGB`/`_SIZE`,
  `APKENV_GL_DEBUG`.

## Where's My Water? (first port)
- **Playable end-to-end with audio**, ships as `com.apkenv.wheresmywater` `.ipk` (launcher icon). Portrait via render-to-FBO; FMOD audio pump. Full writeup: `android-port-shim.md`, `apkenv/BUILD-STATE.md`, `plan/STAGE-*.md`.
- WMW2 (same engine family) reached the level but stalls on multi-threaded GL loading — see `plan/STAGE-5-generalize.md` pt4 (open).

## Conventions
- Temp/scratch work goes in the session scratchpad, **never** this folder.
- Keep candidate `.apk`s pristine; bake any binary patches into a working copy, not the original.
