# CLAUDE.md — Android NDK → webOS game-wrapper workspace

This folder is a workspace for running **Android NDK games** natively on **webOS** (Palm Pre / HP TouchPad) via a **slim apkenv-based wrapper** — **no ACL** (OpenMobile's full Android runtime). The approach: bionic-linker-as-library + fake-JNI + a webOS SDL/PDL backend + a per-game module.

> This is the **Android NDK shim track**, split off from the original `driver` workspace. The sibling **PDK `.ipk`-patching track** (Pre→TouchPad binary patches of native webOS games) lives in its own repo: [https://github.com/webOSArchive/Pre-PDK-to-TouchPad-Ports](https://github.com/webOSArchive/Pre-PDK-to-TouchPad-Ports).

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
- **`plan/memory/`** — the Claude memory for this project, exported into the repo (index:
  `plan/memory/MEMORY.md`). It holds the working rules the user set (systematic not brute force,
  don't touch shipped ports, trust the operator's diagnosis) and the cross-game lessons
  (`android-apk-port-triage` has the candidate verdicts). Claude's live memory is machine-local —
  see *Moving to another machine* below to restore it.

## Resume here (as of 2026-09-17)
Nine games shipped (see the README status table); the project is paused, nothing is in flight.
- **Last session:** RoboCop 1.0.0 released and committed (`plan/ROBOCOP.md`). The README status
  section was rewritten and a repo audit (IP / cruft / onboarding) was run the same day.
- **Open threads, none urgent:**
  - Where's My Water? 2 stalls on multi-threaded GL loading (`plan/STAGE-5-generalize.md` pt4).
  - Aralon's outdoor lighting is slightly less warm than on a Mali tablet — parked by the user.
  - Temple Run 2 / Aralon have not been re-run on a binary with the Unity 4.2 changes. Their shipped
    `.ipk`s carry their own binaries, so this only matters if one is repackaged; the new paths are
    gated off for them. Do one launch test if that happens.
- **Candidates in `android-candidates/` not yet attempted** (verdicts in
  `plan/memory/android-apk-port-triage.md`): **Fruit Ninja 2.1.2** (✅ triaged, Mortar + OBB — the
  most promising), **Bejeweled Blitz** (native SexyApp, OpenSL-only — the OpenSL sink from Tiny Death
  Star now exists), Temple Run 1.6.1 (Unity). Ruled out: Cut the Rope, Flappy Bird, both Space Cats
  (Dalvik/Java), PvZ 2 and PvZ free (freemium, online assets).
- **Method for the next game:** `PORTING-PLAYBOOK.md`, then `apkenv/modules/README.md` (module
  skeleton, the hand-kept `SOURCES` list in `build-webos.sh`, engine → module table) and
  `apkenv/ENV-VARS.md` (all 98 `APKENV_*` switches). The RoboCop trail is the most recent worked
  example of a big Unity title.
- License: BSD 2-clause (`LICENSE`, matching apkenv); third-party licenses in `NOTICE.md`.

## Moving to another machine
**1. Back up what git does not carry.** The repo is the toolkit only; game content is gitignored.
| Path | What | Irreplaceable? |
|---|---|---|
| `android-candidates/` (1.2 GB) | donor apks + OBBs | yes — you can only re-acquire them |
| `apkenv/packaging/extras/pvzhd/data/` (63 MB) | hand-converted PvZ HD OGGs | **yes — no regen script, only copy** |
| `apkenv/packaging/wheresmywater.apk` | the patched WMW apk | no — `apkenv/wmw-patch/bake.py` rebuilds it from the donor (library md5-checked against the shipped one) |
| `apkenv/packaging/extras/wheresmywater2/icon.png` | WMW2 launcher icon | small; keep |
| `apkenv/packaging/out/*.ipk` | the released packages | rebuildable, but they are the known-good builds — keep |
| `apkenv/packaging/*.apk` (other games) | working copies | regen: `tools/rc-stage.sh` (RoboCop), `tools/ds-stage.sh` (Dead Space); the rest are copies of the donor |
| `apkenv/packaging/extras/{templerun2,tinydeathstar,robocop}/` | PCM/splash payloads, RoboCop OBB copy | regen: `tools/tr2-extract-music.sh`, `tools/tr2-extract-splash.sh` (commands in `plan/TEMPLERUN2-RENDER-INPUT.md`, `plan/TINY-DEATH-STAR.md`), `rc-stage.sh` |
| `apkenv/devlibs/libEGL.so` | HP proprietary, harvested | no — `build-webos.sh` re-harvests from a connected TouchPad |
| `plan/logs/`, `apkenv/packaging/out/screenshots/`, `.../icons/` | run logs, grabs, icons | no — nice to have |

**2. Host setup** (Ubuntu 24.04 was the last host):
- PalmPDK at `/opt/PalmPDK` (headers, device libs, `arm-gcc` 4.3.3) and PalmSDK tools on PATH
  (`palm-package`, `palm-install`, `palm-launch`, `novacom`; the `novacomd` service running).
- `sudo apt install gcc-13-arm-linux-gnueabi g++-13-arm-linux-gnueabi` — the compile half of the
  two-toolchain build (it was apt-removed once already; `build-webos.sh` fails with
  "arm-linux-gnueabi-gcc-13: command not found").
- `aapt`, `apktool`, `baksmali`, `unzip`/`zip`, `xz`, ImageMagick (`convert`/`identify`), `python3`.
  For IL dumps of Unity C# assemblies the RoboCop session used the pure-Python `dnfile` + `dncil`
  wheels (`pip install dnfile dncil`).
- Only to rebuild the host Mono runtimes (both are committed prebuilt under `apkenv/hostlibs/`):
  autoconf, automake, libtool, bison, git, and network access —
  `apkenv/tools/build-mono-webos.sh [unity3.5|unity4.2]` clones Unity's Mono fork into `/tmp`.
- On the TouchPad: Developer Mode, USB; `strace` is at `/usr/bin/strace` (managed I/O of the host
  Mono is invisible to apkenv's tracers — see `plan/memory/host-runtime-bypasses-apkenv-hooks.md`).

**3. Restore Claude's memory.** Claude Code keeps it per checkout path in
`~/.claude/projects/<path with / replaced by ->/memory/`. After cloning, copy it back, e.g. for a
checkout at `~/Projects/webos-android`:
`mkdir -p ~/.claude/projects/-home-$USER-Projects-webos-android/memory && cp plan/memory/*.md "$_"`.
When memory changes in a session, re-export it into `plan/memory/` before committing.

## Current state
- **Star Wars: Tiny Death Star 1.4.1 (Cocos2d-x 2.0.4) — ported in ONE SESSION (2026-09-16),
  release candidate 1.0.0**, fresh-install verified: `apkenv/packaging/out/com.apkenv.tinydeathstar_1.0.0_all.ipk`
  (50 MB, the apk carries all assets). Portrait via the ES2 FBO, the game's own LucasArts loading
  splash, touch, FreeType text for `Cocos2dxBitmap`, FMOD music and SFX at 29 fps with zero
  underruns. The user confirmed music and touch on the panel; longer gameplay not yet verified.
  New module: `apkenv/modules/cocos2dx.c` (first cocos2d-x host). Trail: `plan/TINY-DEATH-STAR.md`.
  **Two systemic pieces, both opt-in per module:** `compat/opensles.c` (an OpenSL ES sink: this
  FMOD shipped OpenSL-only; host test `tools/openslestest.c`) and **bionic-layout FILE proxies**
  (`apkenv_bionic_stdio_enable()` in `compat/libc_wrappers.c`). **The lesson worth carrying:**
  bionic's `fileno` is a macro that reads `fp->_file` at offset 14, and a glibc `FILE` has 0 there.
  The game's gnustl `std::ofstream` wrote its 11 MB of sound banks to **stdin** and left empty files.
  Only a `writev` trace showed it. Host note: `gcc-13-arm-linux-gnueabi` was apt-removed on
  2026-09-15; reinstall it for `build-webos.sh`.
- **RoboCop 3.0.6 (Glu, Unity 4.2.2f1 + Mono) — RELEASE 1.0.0 built in one session (2026-09-17)**:
  `apkenv/packaging/out/com.apkenv.robocop_1.0.0_all.ipk` (94.6 MB, OBB bundled). User-confirmed on the
  panel: touch, tutorial, music, aiming, SFX, a full mission, pause/resume and swipe-away; 29.7 fps
  (Unity's 30 cap), zero managed exceptions. The release binary is byte-identical to the tested one;
  a fresh install from the package has not been run. Trail: `plan/ROBOCOP.md`; stage the apk with `apkenv/tools/rc-stage.sh` (the real engine is in
  the OBB). **Systemic pieces, all gated to Unity 4.2 or opt-in:** a Unity 4.2 host Mono
  (`tools/build-mono-webos.sh unity4.2`, exports identical to the game's) with required/optional
  bridge symbols so TR2/Aralon never abort; the **jvalue[] ("A") JNI forms** Unity 4.2's
  AndroidJavaObject uses; a **host WWW** (`nativeInitWWW` + `com.unity3d.player.WWW`: file:// and
  jar:file:// served, network fails offline-style — apkenv had no WWW at all); a **Mono P/Invoke
  fallback** that loads apk libraries through apkenv's linker and runs their `JNI_OnLoad`;
  `APKENV_UNITY_OBB_CODEPATH`, `APKENV_MONO_TRACE`. **Lessons:** managed file I/O bypasses apkenv's
  tracer (host Mono calls glibc) — use the device's `strace`; a "slow internet" screen offline can mean
  a missing LOCAL transport.
- **Dead Space (EA Mobile, BLAST engine) — RELEASED 1.0.0 (2026-09-15)**, fresh-installed from the
  package and verified on a clean profile: `apkenv/packaging/out/com.apkenv.deadspace_1.0.0_all.ipk`
  (174 MB). Launcher icon, 3:2 letterbox (1024x682), menus, 3D intro and cutscenes, audio with zero
  underruns, and **gameplay** — Isaac aboard the Ishimura with the HUD laying out correctly. Module:
  `apkenv/modules/eablast.c`, named for the engine because `com.ea.blast` is EA Mobile's shared
  Android host of that era. Trail: `plan/DEAD-SPACE.md`; contract `plan/deadspace-contract.txt`.
  **Two lessons worth carrying:** (1) `APKENV_TRACE_FILES` now traces `stat`/`opendir` as well as
  `fopen`/`open` — this engine probes before it opens, so an open-only tracer said "it never looks
  for its content" when it was looking and failing; (2) `AndroidEAAudioCore.Init(AudioTrack,III)` is
  `(track, framesPerBuffer, channels, sampleRate)` — rate LAST — and the plausible order made the
  engine generate audio at 8192 Hz while the device drained at 44100, which is the Temple Run 2
  `nativeInit(II)` lesson recurring. Packaging: the engine cannot read content out of the apk, so
  `tools/ds-stage.sh` strips the apk to 2.6 MB and ships `published/` as real files read in place.
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
  Always on in binaries built since 2026-09-14; shipped packages older than that lack it. Before 2026-09-16 the device scripts hung when run from an interactive terminal (fixed: `timeout --foreground`, stdin at /dev/null).
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
