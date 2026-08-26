# PvZ HD (Marmalade/Airplay) — menu freeze: systematic review

**Status (2026-08-26):** **Theory 1 CONFIRMED on device** — the first test logged
`[OSREADSTRING] getInputString title="Enter your name"` → `answered "Player"`, rendering continued,
and the user played Adventure mode (menu → seed picker → level; music switches logged). Stages B+C
(audio) **CONFIRMED**: md5 `059fb4ba` — SFX + music audible, user: "game is playable and sounds great".
Open (§8): mono music tracks need the 44100-stereo transcode (pushed), a thin strip of launcher
wallpaper at the top of the screen, and the 1280x800→1024x768 aspect squeeze. Supersedes the
brute-force trail in `STAGE-5-generalize.md` §8 pt5–pt7 and the memory "FRESH-START PLAN".

## 1. What we were doing wrong

Previous sessions reverse-engineered `libpvz.so` as if it were the game. It is not:

| file | what it is | size |
|---|---|---|
| `lib/armeabi/libpvz.so` | the **Airplay 4.x runtime** (s3e loader, fibre scheduler, JNI bridge to the Java host) | 0x51000 text |
| `assets/PvZ.s3e` | **the game** (PopCap Sexy framework: `LoadTask::MAIN_MENU`, `LoadingBarScreen`, `s3eSoundManager`…), LZMA-Alone | 3.4 MB decompressed |
| `assets/PvZ.dz` | resources (Derbh) — already cracked (`tools/derbh_extract.py`) | 32 MB |

So every gate/state-machine/fibre patch aimed at `libpvz` addresses was patching the *host*,
not the code that was actually waiting. The fibre-stack scanner also filtered return addresses
to the libpvz range, which silently discarded every frame of the game.

The correct frame: **the game runs on an s3e fibre; the runtime's main loop pumps it; the game
polls things through s3e → JNI call-outs into the Java `AirplayThread`/`AirplayView` host.**
apkenv's `modules/marmalade.c` *is* that host. Any Java method the engine calls that the module
does not implement returns 0/NULL silently — and if the engine blocks on that result, the
symptom is precisely "renders, then spins on `deviceYield` forever".

## 2. The host contract (from `baksmali` of the apk's `com.ideaworks3d.airplay.*`)

Engine → host methods libpvz can call (89 names found as strings) vs. what `marmalade.c`
implemented before this pass: **26**. Unimplemented and *blocking-capable* ones:

| method | Android behaviour | engine-side wait | risk |
|---|---|---|---|
| `getInputString(String,int)` | posts an AlertDialog+EditText; `doneInputText` → native `setInputText` | **`while(result==NULL) deviceYield(20)`** at libpvz `0x25b0e–0x25b2c` | **HIGH — this is the freeze** |
| `showError(String,String,int)` | modal dialog, returns button | spin on `m_ErrorRtn` | high (any assert/error path) |
| `audioGetStatus/audioIsPlaying/audioGetPosition` | MediaPlayer state | game-side polling (music transitions) | medium |
| `videoPlay/videoGetStatus` | MediaPlayer video | `videoStoppedNotify` | low (PvZ HD has no video) |
| `touchSetWait`, `onDoneResume`, `loaderLock/Unlock`, `hasKeyboard`, `getSilentMode`, `backlightOn` | trivial | none | none |

Semantics that differ but are **not** blockers (verified against the smali):
- `deviceYield(ms)`: Android `Object.wait(ms)` (ms<0 → wait forever until `deviceUnYield`); we
  return immediately → busy-spin, harmless except CPU.
- `suspendAppThreads`/`resumeAppThreads`: called by **Java's** `surfaceChanged/Destroyed` on the UI
  thread; `suspendAppThreads` parks *the caller* until the engine reaches a safe point
  (count `+0x148`==0, which the engine signals around its JNI call-outs). We never call it, so
  the whole pt5–pt7 "app threads left suspended" model was inverted — there was never anything
  to resume. The `futex_wait` worker seen on-device was SDL's audio thread.
- `SoundPlayer.generateAudio`: Android runs a Java AudioTrack thread calling it; we open the
  mixer but never pump it (`Mix_SetPostMix` TODO) → sound effects silent (Stage C).

## 3. Root cause (Theory 1 — confirmed statically, one device test to close)

1. Previous device logs already recorded, on the tap that "froze" the menu:
   `[MARMCALL] getInputString` (memory entry 2026-06-29).
2. `PvZ.s3e` strings: `"Enter your name"`, `s3eOSReadString` — PvZ prompts for the profile name
   on first run, right after "tap to continue" (it's the PC game's "Who are you?" dialog).
3. `libpvz 0x25a9c` (s3eOSReadStringUTF8): frees+clears result slot `[state+0x154]`, calls Java
   `getInputString(title, flags)`, then loops `if(quit) break; deviceYield(20); while(slot==0)`.
   `AirplayView.setInputText` native (`0x23a94`) = `GetStringUTFChars → malloc → strcpy → slot`.
4. apkenv's `CallVoidMethodV` had no `getInputString` case → slot stays NULL → engine spins on
   `deviceYield` with rendering stopped. Matches every observed signal: main thread R on
   deviceYield, swap frozen, no file I/O, no thread stuck, memory irrelevant, "freeze frame"
   varying with *when the user tapped*.

This is a **general contract gap** (any Marmalade/Airplay game using `s3eOSReadString` would
freeze the same way), not a PvZ hack — it belongs in the runtime.

## 4. What was built (all in `apkenv/modules/marmalade.c`)

1. **`getInputString` → `setInputText` answer** (`marm_answer_readstring`): logs
   `[OSREADSTRING] getInputString title="…" flags=…`, answers synchronously with
   `APKENV_MARM_READSTRING` (default `"Player"`; never empty — PvZ re-prompts on empty), logs
   `[OSREADSTRING] answered "…"`. Race-free: the engine clears the slot *before* the call.
2. **Unhandled call-out tracer** (`marm_trace_unhandled`, always on): every engine→host method
   we don't implement prints `[MARM-JNI] UNHANDLED <void|int|bool|obj> <name><sig> (swap=N)` once,
   then at 100/10k/1M calls. Any future freeze of this class now names itself in the log.
3. **`audioPlay` signature dispatch**: Airplay is `(String,int)`, Marmalade `(String,IJJI)`; the
   old code read two garbage `long long`s + channel off Airplay's 2-arg va_list. Now keyed on
   `method->sig`; logs `[MARM-AUDIO] audioPlay '<path>' …` and `-> rc`; a missing track returns
   -1 (Android FileNotFound semantics) instead of passing NULL to the mixer.

Nothing from the failed experiments was removed yet (RAMPATCH/GATEBYPASS/COOPYIELD/FIBREDUMP/
stall-resume are still env-gated or harmless); they get stripped in Stage D once Theory 1 is
confirmed, so the test compares exactly one change against the last deployed build.

## 5. Test protocol (needs the TouchPad; ~3 minutes)

Prereq: device on the LAN as `192.168.10.88` (it was unreachable when this was written).

    # from the repo root
    apkenv/tools/deploy-tp.sh apkenv/apkenv /var/apkenv2/apkenv     # pipes over legacy ssh
    # on the device (or via ssh): reboot first if free RAM < ~450 MB (free | head -2)
    sh /var/apkenv2/play-pvz.sh                                        # unchanged launcher

Then: wait for the loading screen → tap "tap to continue" → **observe**.

Expected if Theory 1 is right:
- log shows `[OSREADSTRING] getInputString title="Enter your name" …` followed by
  `[OSREADSTRING] answered "Player" via setInputText`;
- the swap counter keeps advancing and the **main menu appears and responds to taps**;
- Profile name shows as "Player" on the menu's welcome banner.

If it still freezes: the `[MARM-JNI] UNHANDLED …` line nearest the freeze names the next gap
(most likely `showError`, or an `audioGetStatus` poll) — that is the next theory to stage, not a
reason to touch libpvz addresses again.

Report back: the last ~30 log lines (`[OSREADSTRING]`, `[MARM-JNI]`, `[MARM-AUDIO]`) + whether
the menu responds.

## 6. Staged theories after Theory 1

| stage | theory | signal to look for | fix location |
|---|---|---|---|
| **B** music | `s3eAudioPlay` → `audioPlay('music/xxx.mp3')`: path is apk-relative (`assets/`), not on the extracted tree → `load_music FAILED`; game may poll `audioGetStatus` | `[MARM-AUDIO] …FAILED`, `[MARM-JNI] UNHANDLED int audioGetStatus` | asset-root fallback in `audioPlay`; implement `audioGetStatus/IsPlaying/GetPosition` from mixer state |
| **C** sfx | `generateAudio` never pumped (mixer post-mix TODO) → silent effects | no `generateAudio` activity; user hears no sfx | wire the mixer callback to `soundplayer.generateAudio` (same shape as WMW's FMOD pump) |
| **D** cleanup | strip the false-premise code: stall-triggered `resumeAppThreads`, COOPYIELD, RAMPATCH, GATEBYPASS, FIBREDUMP/STACKSCAN, PWATCH | — | `marmalade.c`, `compat/pthread_wrappers.*`; keep the JNI tracer |
| **E** input | real text entry for `getInputString` (TouchPad keyboard via PDL/SDL text events) instead of the default name | — | module-level, contract-preserving |
| **F** dialogs | `showError` → on-screen message + auto-OK, to keep error paths from freezing | `[MARM-JNI] UNHANDLED int showError` | module |

## 7. Stages B+C — audio (built 2026-08-26, md5 `2b534702`)

Contract (from `SoundPlayer.smali` / `AirplayThread.smali`):
- **SFX**: `soundInit(boolean stereo, int rate) → rate`; an AudioTrack thread calls
  `generateAudio(short[] buf, nFrames)` per period and writes `nFrames*channels` shorts.
- **Music**: `audioPlay(String path, int repeats)`; `repeats==0` ⇒ `setLooping(true)`;
  `audioGetStatus` 1=Started 2=Paused 3=Error; `audioSetVolume(int pct)`; `audioStop()`.
  Path arrives as `media/internal/.apkenv/pvzhd.apk/music/x.mp3` (no leading slash — Android's
  cwd is `/`). The TouchPad's `libSDL_mixer` has **vorbis only, no MP3 decoder**.

What was wrong: the mixer was opened but `generateAudio` was never pumped (`Mix_SetPostMix` TODO);
`sound_volume` defaulted to 0; `audioPlay` used Marmalade's 5-arg va_list and `repeats<0` loop
rule; `sdl_mixer_load_music` returned a non-NULL wrapper around a failed `Mix_LoadMUS`.

Built:
1. `mixer.set_postmix` hook (SDL: `Mix_SetPostMix`) + `my_audio_mixer` rewritten as the pump:
   asks the engine for exactly the callback's frames and mix-adds them to the music stream.
   Logs `[MARM-SOUND] soundInit …`, `soundStart`, `generateAudio cb#N frames= peak=`.
2. `soundInit` dispatched by signature (`(ZI)I` Airplay vs `(IZI)I` Marmalade), mixer opened at the
   engine's rate/channels.
3. `audioPlay`: path candidates = as-is, `/`+path, and `.ogg` twin of each; loop per loader
   semantics; `load_music` failure now really returns NULL. Music transcoded offline
   (`ffmpeg -c:a libvorbis -q:a 3`) and pushed next to the MP3s in
   `/media/internal/.apkenv/pvzhd.apk/music/`. `audioGetStatus/IsPlaying/SetVolume/Stop` implemented.

Test: launch, tap through to the menu. Expect `[MARM-SOUND] soundInit` + `generateAudio cb#1 …
peak>0` once an SFX plays, `[MARM-AUDIO] loaded '…/crazydave.ogg'`, and audible music + effects.
If music loads but is silent: check `audioSetVolume` lines; if `peak=0` always: the engine isn't
producing SFX (look for `[MARM-JNI] UNHANDLED` sound-related calls).

### Audio results (2026-08-26, md5 `059fb4ba`)
- First audio build asserted in `sdl_mixer_open`: a **stale object** (`mixer.c` not rebuilt after the
  `struct Mixer` change) — `build-webos.sh` has no header dependency tracking; `rm build/webos/*.o`
  after touching a shared header.
- Engine asks `soundInit(stereo=0, rate=0)`; `rate=0` = "native". The TouchPad's SDL_mixer (it has an
  ffmpeg MP3 decoder after all) refuses to resample or remix music: music must exactly match the
  opened device. So the mixer is always opened **44100 Hz stereo**, the pump widens the engine's mono
  frames, and music is transcoded offline to 44100/stereo OGG (`ffmpeg -ac 2 -ar 44100`). Stereo
  MP3s load directly; mono ones (crazydave, chooseyourseeds…) hit "Mixer number of channels not
  compatible" until the stereo OGG twins were pushed.
- `[MARM-SOUND] generateAudio cb#N` confirms the pump; `peak` is only sampled every 2000 callbacks.

## 8. Open items after audio
| item | observation | first step |
|---|---|---|
| top strip | a thin band of launcher wallpaper above the game; not seen on the first run | log the engine's `glViewport`/`glScissor` calls + `setPixelsNative` size; check whether the engine renders 1024x(768-n) (letterbox) or our present leaves rows unpainted |
| aspect | assets exist only as `1280x800` (16:10) → squeezed to 4:3 | likely engine-side scaling; options: letterbox via viewport (cost: smaller image) or accept |
| Stage D | strip false-premise code (stall-resume, COOPYIELD, RAMPATCH, GATEBYPASS, FIBREDUMP, PWATCH); keep `[MARM-JNI]` tracer | after a clean confirmation run |
| Stage E | real name entry (TouchPad keyboard) instead of "Player" | module-level |
| packaging | **DONE (build)**: `com.apkenv.pvzhd_1.0.0_all.ipk` (142 MB) — see §9 | install + launcher test |

## 9. Letterbox + packaging (2026-08-26)
- **Letterbox CONFIRMED** (md5 `122cdb55`): `APKENV_MARM_LOGICAL=WxH` makes the module report a
  centered surface of that aspect (1280x800 on 1024x768 → 1024x640 at +0,+64); new generic
  `module_hacks.viewport_offset_{x,y}` shifts every real-framebuffer `glViewport`/`glScissor`;
  touches are shifted the same; `apkenv_gles_clear_screen()` paints the bars black after each swap
  (this also removed the wallpaper strip). User: "it looks great".
- **Packaged launch generalized** (`apkenv.c`): per-app log `/media/internal/apkenv-<appid>.log`;
  `android/apkenv.env` KEY=VALUE lines are `setenv`'d at launch (no shell in the PDK jail);
  `android/<apk>.data/` is copied once into `/media/internal/.apkenv/<apk>/` (marker
  `.apkenv-seeded`) — FAT can't symlink and the jail has no tar, so it's a C tree copy.
- `packaging/build-ipk.sh` takes `DATA=` (tree) and `ENVFILE=`; PvZ inputs live in
  `packaging/pvzhd/` (`appinfo.json` with `requiredMemory: 400`, `apkenv.env`). Bundle = apk +
  extracted tree with MP3s replaced by 44100-stereo OGGs (63 MB) → 142 MB `.ipk`.
  Build: `APPID=com.apkenv.pvzhd APK=packaging/pvzhd.apk DATA=<tree> ENVFILE=packaging/pvzhd/apkenv.env APPINFO=packaging/pvzhd/appinfo.json packaging/build-ipk.sh`;
  install: `palm-install` over USB (first attempt was cut off mid-copy → remove + reinstall).
- First launch from the icon copies ~490 files (63 MB) before the game starts — expect a pause.
- **Launcher-icon launch CONFIRMED** (2026-08-26): packaged run logged env → bundled apk → seeding 489
  files → game; user played it. Title changed to "Plants vs. Zombies", version 1.0.1
  (`packaging/out/com.apkenv.pvzhd_1.0.1_all.ipk`, 142 MB).
- Sharing note: unlike the WMW package, this `.ipk` bundles the copyrighted apk + extracted
  resources. For public distribution, the WMW approach (runtime-only .ipk, user supplies the apk on
  /media/internal) is the safe form; the seeding/env machinery works either way.
