# Aralon: Sword and Shadow HD 4.53 (Unity 4.0.1f2 + Mono) — port plan

Donor: `android-candidates/Aralon/00757-Aralon-Sword-and-Shadow-4.53.apk` (pristine; md5
`d0572f2c66d50fbacfe18899e5c6346a`) + `main.10.com.crescentmoongames.aralon.obb` (286 MB, from the
`…obb-cache.zip`; md5 `2c1553626cb3522523c2e4e12cd03c64`). The `-mod` apk is an IAP crack only
(prime31 IAB + `IInAppBillingService` + firstpass.dll; the IAPs are six consumable "karma" packs) —
**not used**. Working copies: `apkenv/packaging/aralon.apk`, `apkenv/packaging/extras/aralon/`
(both gitignored).

## Why this is mostly the Temple Run 2 stack again (static, 2026-09-14)

| Question | Answer | Evidence |
|---|---|---|
| Engine | Unity **4.0.1f2** (TR2: 3.5.7f6), same proxy layout — the engine is `assets/libs/armeabi-v7a/libunity.so` | `mainData` header |
| Mono bridge | Our `hostlibs/webos/libmono-webos.so` exports all **119** Mono symbols libunity imports; Aralon's bionic Mono has 5 extra exports that libunity never names | `comm` of UND/DEF sets |
| Corlib | **82** in Aralon's Mono, TR2's Mono and our host Mono → Aralon's own (different) `mscorlib` passes `mono_check_corlib_version` | `cmp r2, #82` in each |
| Audio | `org/fmod/FMODAudioDevice.smali` byte-identical to TR2's → same pump | md5 |
| Java↔native | natives identical except `unityAndroidInit` returns **Z** and new `nativeSetInputCanceled(Z)` | smali diff |
| Host contract | +6 names vs TR2, none blocking; all other unhandled names were unhandled under TR2 too | see below |
| Display | `sensorLandscape` → native landscape, no FBO rotation (`APKENV_UNITY_PORTRAIT=0`) | manifest |
| Textures | no `supports-gl-texture` → Unity default ETC1 (native on the Adreno's ES2 context) | manifest |

## The Unity 4 host contract, read from `UnityPlayer.smali` (not assumed from TR2)

```
<init>:        nativeFile(getPackageCodePath())  ->  l(): for each obb: nativeFile(obbPath)
a(IZ):         a(Z) wakelock, c() nativeForwardEventsToDalvik, initJni, PlayerPrefs, nativeInitWWW
UnityPlayer$25 nativeInit(glesMode = a(IZ)'s p1 = settings gles_mode (2),
                          splashMode = getSettings().getInt("splash_mode") (Aralon: 0; TR2: 1))
onSurfaceCreated -> nativeRecreateGfxState
onDrawFrame:   nativeRender(); first time: i() = unityAndroidInit("assets/bin/", dataDir+"/lib")
                                             -> nativeResize(w,h,w,h) -> unityAndroidPrepareGameLoop
               then nativeResume -> windowFocusChanged(true)
```
**Difference from TR2 that matters:** Unity 4 resizes *between* `unityAndroidInit` and
`unityAndroidPrepareGameLoop`; Unity 3.5 resized *after* prepare. `modules/unity.c` now follows
whichever host the engine is (detected by `nativeSetInputCanceled` in its native table).

New host methods answered explicitly: `getScreenOrientationAngle`→0, `getCameraOrientation`→0,
`vibrationSupported`→false, `isSensorCompensationEnabled`→false, `enableSensorCompensation`/
`setSoftInputStr`→no-op. `getPackageName` → `APKENV_UNITY_PACKAGE`.

## Ranked risks (theories for when something fails)
1. **Mono runtime/corlib subtleties** — same corlib version, but a different Unity Mono revision.
   Symptom: fault in `mono_*` during `unityAndroidInit`. Fallback: rebuild host Mono from the Unity
   4.0 mono branch (`tools/build-mono-webos.sh`, repin) and re-diff exports.
2. **OBB not seen** — symptom: engine logs missing `mainData`/level files after `unityAndroidInit`.
   Check `[UN] nativeFile(obb …)` line; the Java host also gates on a settings key (the md5-named
   `630fc…` bool) that we do not reproduce — native side may check its own.
3. **FMOD music stream stall (TR2's)** — same glue; TR2's single-bed workaround is weak for a
   per-area soundtrack. Measure before designing.
4. **Multi-touch** — plumbing passes pointer ids 0..4; TR2 never exercised a second finger.
5. **RAM** — 512 MB of data for an open-world RPG; `requiredMemory` 400 to start.

## Stage A — first boot (protocol)
Build + package:
```
cd apkenv && ./build-webos.sh
APPID=com.apkenv.aralon APK=packaging/aralon.apk APPINFO=packaging/aralon/appinfo.json \
  ENVFILE=packaging/aralon/apkenv.env HOSTLIBS=hostlibs/webos EXTRAS=packaging/extras/aralon \
  packaging/build-ipk.sh
APPID=com.apkenv.aralon WAIT=60 tools/tr2-run.sh aralon-a1
```
Expected log lines, in order: `[UN] unity4 host: yes` → `nativeFile(<apk>)` → `nativeFile(obb …
286 MB)` → `121/121`-style hostlib bridge count → `nativeInit(glesMode=2, splashMode=0)` →
`[UN-GLES2] … NOT patching` (TR2-only offsets; expected) → `unityAndroidInit … -> 1` →
`nativeResize` → `prepare done` → `nativeRender #1..3`. A snapshot (`APKENV_GL_SNAPSHOT`) decides
"renders" without the user. Pass = frames advancing with non-zero draws; the user is only needed
for touch (Stage B).

## Log
- 2026-09-14: triage + static contract (above); unity.c Unity-4 order, OBB, 6 host methods, package name.
- **A1 (`logs/aralon-a1.log`)**: env applied, packaged launch OK, `[HOSTLIB] 121/121 symbols
  bridged` — then `cannot locate 'mono_string_new_len'… failed to link libunity.so`. The bridge list
  (`compat/mono_symbols.h`) was TR2's libunity∩Mono set; Aralon's libunity also imports
  `mono_string_new_len` + `mono_object_get_virtual_method` (the apk libmono is blacklisted, so there
  is no fallback). The static check "all 119 exported by the host Mono" was true and beside the
  point: exported ≠ bridged. Fix: per-game lists `plan/{tr2,aralon}-mono-imports.txt`,
  `tools/gen-mono-hooklist.sh` emits their union (123). Early `Unimplemented: __pthread_clone` and
  `GLES2 table … (live context is ES1)` lines are identical in TR2's good log — normal.
- **A2 (`logs/aralon-a2.log`)**: bridge 123/123 → **Mono up, OBB accepted, `unityAndroidInit ->
  1`, engine chose its SHADER renderer by itself** (no TR2-style device patch needed; the TR2
  offsets are correctly refused). Then SIGSEGV addr=0x14 at `libunity+0x37a914`, inside
  `unityAndroidPrepareGameLoop`'s one-time setup (`+0x37a7ec`), which dereferences the global at
  `+0x78883c` without a check. Found statically (native-table decode + a pc-relative-xref scan of
  the disassembly): the only writer is `+0x3733c0`, a lazy builder of the loading-screen blitter
  (`glGenTextures` + a shader), reached only from `nativeResize` **while the engine is not yet
  initialised** (`+0x37a77c beq` → tail-call `+0x37378c`). The Unity 4 host always takes that branch
  once, because `onSurfaceChanged → nativeResize(w,h,viewW,viewH)` and the first `onDrawFrame`'s
  leading `nativeRender()` both run before `i()`. unity.c resized only after init. Fix: Unity-4
  path now does `nativeRecreateGfxState → nativeResize → nativeRender → unityAndroidInit →
  nativeResize → prepare`, i.e. the host's order. **Lesson: a native's behaviour can depend on
  engine state, so "we call it" is not "we call it when the host does".** Also capped the
  `eglGetProcAddress: unimplemented` line (Unity 4 re-asks for the NV timers every frame).
- **A3 (`logs/aralon-a3.log` + GL snapshots)**: boots, tiny Unity splash, then the forest
  main-menu scene renders smoothly at 1024x768 — **with no menu UI, taps ignored, no sound**. Taps DO
  reach `nativeTouch` (`[UN-TOUCH] … src=0x1002`); Unity 4's Java touch path is identical to 3.5's.
  The game's own log: `JNI: Unable to find method id for 'getMetrics'`, `… field id for
  'widthPixels'` etc., `Init'd AndroidJavaObject with null ptr!`. IL scan (`scratchpad/ilscan.py`,
  built on `tools/ildump.py`): `DisplayMetricsAndroid..cctor` does
  `currentActivity.getWindowManager().getDefaultDisplay().getMetrics(new DisplayMetrics())` via
  AndroidJavaObject, and **`GuiMgr:configure` sizes the whole GUI from `WidthPixels/HeightPixels`**;
  `GuiMgr:useLargeScreen` uses `WidthPixels/XDPI`. All read 0 under us → a zero-size GUI. Cause in the
  shim: Unity's `ReflectionHelper.getMethodID/getFieldID/getConstructorID` unanswered,
  `FromReflected*`/`Call*MethodA`/`NewObjectA` return NULL, field reads return 0, and unity.c's
  `NewGlobalRef` returns NULL for any real object. Fix: a Unity-4-gated AndroidJavaObject bridge in
  unity.c (reflected-member objects → IDs; made-up objects for WindowManager/Display/DisplayMetrics;
  `getClass().getName()`; metric fields = 1024x768, density 1.0/160, xdpi 132).
  Audio: FMOD pump + 24 kHz AudioTrack open, nothing audible — next after the UI.
- **A4 (`logs/aralon-a4.log`) — MENU + TOUCH WORK.** The bridge answered the whole chain
  (`new DisplayMetrics` → `currentActivity.getWindowManager` → `getDefaultDisplay` →
  `getClass().getName()`=`android.util.DisplayMetrics` → `getMetrics` → 1024x768 / 1.0 / 160 / 132 dpi);
  the game switched from `GuiText/low/` to **`GuiText/large/`** fonts and drew New Game / Options /
  More Games. User tapped New Game (`Command: New`) → intro cutscene (fireplace, Skip button) →
  character creation loading (`player/HumanMale`, `ElfMaleHair1`). Remaining gaps: **no audio**;
  prime31 IAB `instance()`/`init` still unresolved (IAP only, harmless); the jclass passed to
  ReflectionHelper for `currentActivity`/`instance` is not a `dummy_jclass` (logs as `,>`) — dispatch
  is by name so it does not matter yet, but note it if a lookup ever needs the class.
  Next: audio run with `APKENV_AUDIO_METER=1 APKENV_THREAD_SAMPLE=1` (dev env prepared).
- **A5/A6 — AUDIO: SFX + dialogue + ambience WORK, music does not.** `APKENV_AUDIO_METER=1`: real
  peaks (10k–28k) on clicks and dialogue, `0` between them at the menus → the pump/AudioTrack path is
  healthy (the "no sound" in A3 was only because there was nothing to click and the intro's audio is
  music). Evidence music never *starts* (≠ TR2, where the stream primed 64 KB then stalled):
  `/proc/<pid>/fdinfo` sampled every 3 s (non-invasive, no restart — `scratchpad/fdsample*.sh`)
  shows the OBB is read through ordinary fds only while loading (positions inside deflated
  `level*`/`sharedassets*.split*`), the apk alone is mmapped, and **no fd ever sits in any of the 11
  stored MP3 `.resS` ranges** — menus, character creation, in-world indoors and outdoors.
  Static: music = `ZoneMusicMgr` (`Awake`: `Resources.Load("music/"+zoneMusicClipName [+"Low" iff
  PlayerOptions.DEVICE_GEN < 3])` → `PlayMusic` → `Invoke("SetCurrentMusic")` → `clip=…; Play()`).
  DeviceGen logs 3 → full-quality names. Resource table (apk `mainData`) lists `music/theme`,
  `outdoors..outdoors4`, `village`, `city`, `combat`, `dungeon` + full tracks and `…low` variants;
  `sharedassets0` has a `MusicMgr` object. Refuted: music volume 0 (`GetFloat("MusicVolume", 0.25)`).
  One `Can not play a disabled audio source` after a dialogue burst — looks like a cutscene
  `DelayedAudio`, not music. Also seen: a long black line from a bird (trail/line renderer?) and cyan
  speckles in the sunset clouds (user screenshot `/media/internal/screencaptures/aralon_*091703.png`).
  Next (A7): `APKENV_TRACE_FILES=1` + `MONO_VERBOSE_METHOD=ZoneMusicMgr:SetCurrentMusic` — does the
  Play path run, and does the stream's file open fail (OBB-resident `.resS`)?
- **A7 + static music analysis.** A7 (`APKENV_TRACE_FILES=1`, `MONO_VERBOSE_METHOD=
  ZoneMusicMgr:SetCurrentMusic`): no failed opens except `/sys/devices/system/cpu/present`; the OBB
  opens fine; the success-trace cap (400) was hit at line 1567, so late opens went unlogged; the
  Mono probe printed nothing — **but it ran without a positive control** and used the unproven
  `Class:Method` form (TR2 only proved the bare-name form, `=Awake`), so "never called" is NOT
  established. Outdoors the meter reads a steady 450–3,600 (ambience, or music at 0.25?). The
  static answer is firmer: `PlayerOptions.optionMusic = {On(OPTION_ON=1), Off}`, default index 0
  (`IntValueCycler` ctor), `LoadOptions` = `setIndex(GetInt("Music", getIndex()))` → **music ON on
  a fresh profile**. `ZoneInfo:PostLoadGame`: `if (zoneMusic != 6 && optionMusic != OFF)
  zoneMusicMgr = Instantiate(Resources.Load("music/"+musicPrefabs[zoneMusic]))` → ZoneMusicMgr
  `Awake → Resources.Load("music/"+zoneMusicClipName) → PlayMusic → Invoke(SetCurrentMusic) → Play`.
  Correction to the fd evidence: not every music clip is a `.resS` stream — `theme` (menu) has no
  `.resS`, so "no fd in a .resS range" never covered it. Music prefabs are GUID files pairing a prefab
  name with a clip (e.g. `outdoors` → `simple_lives_full-slow`, data in `9c2a9203….resS`).
  **Working hypothesis:** music = FMOD *streams* (compressed MP3, memory or `.resS`), SFX/dialogue =
  decoded samples — the same split as Temple Run 2, whose streams never play under this host
  (unsolved there; worked around). To confirm with ONE run: `MONO_VERBOSE_METHOD=PlayMusic`
  (bare-name form = the proven one), plus read accounting over the `.resS` ranges.
- **A8 (`logs/aralon-a8-live.log`) — the music code DOES run.** `MONO_VERBOSE_METHOD=Awake`
  (bare-name form, proven in TR2): 26 `Awake` methods compiled incl. `GUIMainMenu:Awake`,
  `ZoneInfo:Awake` (positive control) and **`ZoneMusicMgr:Awake`** (line 24718, in HouseStart,
  outdoors), and no `music clip is null` → the zone music prefab is instantiated, its clip loads and
  `PlayMusic` runs. The game asks for music; the user hears ambience only. The multi-range read
  accounting (new: `APKENV_TRACE_SEEK_RANGE` takes up to 16 comma-separated ranges, per-range
  totals) did NOT run: **the binary push failed** (`novacom put` → `file open failed`, the killed
  process still held the file) and the script launched anyway with the old binary + new env. Fixed
  in the procedure: kill until `pidof` is empty, push, **refuse to launch unless the device md5
  matches**. A9 repeats the read accounting with the right binary.
- **A9 (`logs/aralon-a9-live.log`, correct binary: md5-gated push, `[READ] watching 11 range(s)`).**
  `ZoneMusicMgr:Awake` again in HouseStart; user outdoors, ambience only. Every range gets a 30-byte
  read exactly at its end = the zip local header of the NEXT entry (archive scan from
  `nativeFile(obb)`; stack `libunity+0x3857d8` is inside `nativeFile`) — not music. **Only range #8
  (`sharedassets1.assets.resS`, 5.2 MB, begins with an ID3/MP3) is really read**: sequentially in
  2 KB steps, whole file, ~4 passes on fresh fds (47, 46, 46, 40), ~19 MB, from log line 96 (before
  the first frame) to 25798 (after ZoneMusicMgr:Awake). ~4 passes ≈ 4 scene loads (menu, character
  creation, HouseStart ×2), so the likelier reading is *scene resource loading of a .resS that starts
  with an MP3*, not a looping music stream — ambiguous, not claimed. No other music `.resS` is read at
  all. Firm: the game requests zone music; nothing audible; no zone track's stream data is read
  during play. **Next: reference run on a real Android tablet** (original apk + OBB, `adb logcat`
  for Unity/FMOD) to learn what plays where, then diff.
- **ANDROID REFERENCE (HP 10 G2 Tablet, Android 5.0.1, original apk + OBB via adb;
  `logs/aralon-android-ref.logcat`).** Music plays on the main menu, all menus, the opening cutscene,
  and quietly outdoors — **all missing on the TouchPad; SFX/dialogue/ambience match.** Same startup
  log as ours after the DisplayMetrics fix (`DeviceGen: 3`, `Use touches`, `GuiText/large/`), same
  zone flow (`cut scene is valid` → `Transition to zone: ContSouthStart` — the TouchPad does this too;
  an earlier note that it stayed in HouseStart was a grep mistake). **Audio output is identical:** one
  Java AudioTrack, 24000 Hz stereo S16, 2048-byte writes ~47/s from the `FMODAudioDevice` thread —
  exactly our pump's shape. So music is missing from FMOD's mix, not lost on output.
  Refuted, in order: (1) volume 0 — default 0.25; (2) option off — `optionMusic` defaults On;
  (3) music never requested — `ZoneMusicMgr:Awake` runs (positive-controlled); (4) bridged libm —
  all 34 math imports resolve from bionic `libm.so`, no glibc seam; (5) Android decoding MP3 via
  OpenSL/platform codec — libunity has FMOD's own `FMOD MPEG Codec`, its OpenSL code is an *output*
  plugin, and Android logs no OMX/MediaCodec activity; (6) the idle `FMOD stream thr` — it is idle on
  Android too (0 CPU ticks while music plays); (7) `asyncProcessor` lost wakeups — it is Unity's
  thread (entry `+0x37cc90` names itself), and libunity's sync imports are consistent (all `sem_*`
  bionic, all cond/mutex via apkenv's glibc wrappers). Also: our liblog shim prints every priority,
  so Android's three generated fixed-function lighting/fog shaders (logged `D` on entering the world)
  really are never generated on the TouchPad — a lead for "Android outdoors is greener".
  **The one FMOD-internal difference found:** Android has an `FMOD file thread`; the TouchPad never
  creates it. Its creator (`+0x47d760`, FileThread init) is reached lazily from FMOD's File open path
  (`+0x43bb54` → `+0x439260` → `+0x47d9b0`). FMOD created exactly one thread via its helper
  (`+0x485828`) on the TouchPad: the stream thread. **Hypothesis for A10:** webOS lacks
  `/sys/devices/system/cpu/present` (fopen FAILED in every run); Unity then assumes 1 CPU (0
  `UnityWorker` threads vs 3 on Android) and FMOD plausibly takes a single-CPU path without the file
  thread. Test: `APKENV_SYS_CPU=1` (libc_wrappers.c answers present/possible/online from the real core
  count), with meter + thread sampler + 11-range read tracer. Expected if right: `UnityWorker` and
  `FMOD file thread` appear, a `.resS` range is read steadily, the meter shows continuous music.
- **A10 (`logs/aralon-a10-live.log`) — CPU-count hypothesis REFUTED.** `APKENV_SYS_CPU=1` took
  effect (`[SYSCPU] … -> 2 CPU(s)`; Unity now starts one `UnityWorker`, cores−1, as Android does
  with 4 cores → 3), but there is still **no `FMOD file thread`**, the menu meter is a solid `0`, and
  the only music-range reads are the scene-load passes over `sharedassets1.assets.resS`. Full log
  (`logs/aralon-a10.log`) confirms: 0 `FMOD file thread` in any thread sample, `UnityWorker` in all
  29, meter `0` in all 76 windows (~2.5 min) — a menu-only run (no `Command:` lines). Keep the
  sysfs answer (it is what an Android kernel provides) but it is not the music fix; it stays opt-in.
  State after ten runs: the game requests music (positive-controlled), SFX/dialogue/ambience play
  through the same FMOD mixer and AudioTrack path Android uses, and FMOD never opens a music stream
  on the TouchPad (no file thread, no progressive `.resS` reads) where Android does.
- **A11 — THE MUSIC CLIPS LOAD EMPTY.** New instrument: `compat/icall_trace.c`
  (`APKENV_UNITY_ICALL_TRACE=1`) wraps `mono_add_internal_call` — bridged from our host Mono, so it
  is ours — and puts logging trampolines in front of 12 audio icalls (≤4-word signatures, forwarded
  bit-exactly; returns as `long long` to keep r0:r1). Installed before the bridge, which leaves
  hooked names alone. Why this boundary: libunity exports FMOD's C++ API (330 symbols) but calls it
  directly, never via its PLT, so FMOD cannot be interposed. Result at the main menu:
  `AudioSource.Play(src=0x2d9bec80) clip=0x2d9c9c90 len=0.0s ready=1 -> isPlaying now=0` (twice,
  `set_volume 0.25`), and a second music source likewise `len=0.0s`. **Positive control:** the button
  click via `PlayOneShot` reports `len=0.2s ready=1`, so the length readout is right. No Unity audio
  error is printed (Unity does print them — `Can not play a disabled audio source`), so FMOD did
  not refuse the sound; the clip is *ready with zero length*: Unity appears to hand FMOD no audio
  data for music clips, although the whole MP3 `.resS` is read from the OBB. Next: Unity's
  AudioClip → `FMOD::System::createSound/createStream` path (direct call sites) — where the music
  clip's data pointer/length come from and where they are lost.
- **Static, after A11 — where the music is lost.** All FMOD sounds come from four
  `FMOD::System::createSound` call sites (`createStream` is never called). Mode bits, decoded with the
  real FMOD Ex table (an earlier table of mine mislabelled several bits): `+0xd79dc` = `OPENUSER|
  SOFTWARE|2D` (user/record sound); `+0xdbd38` = `OPENUSER|CREATESTREAM|…` with a PCM read callback
  (callback-fed streams); **`+0xdbf74` = the asset loader** (func `+0xdbe80`, single caller
  `+0xdc0e8` in `+0xdbfd0`, the AudioClip load): exinfo carries Unity's own file callbacks
  (useropen/close/read/seek = `+0xd0320/+0xd0420/+0xd023c/+0xcf090`), the suggested sound type, and
  for type 13 (MPEG) adds **`MPEGSEARCH`** — FMOD scans the whole MP3 at load, which is what A9's
  four full passes over `sharedassets1.assets.resS` were. **On failure the loader calls FMOD's
  error-string function, stores the string at AudioManager+156 and the FMOD_RESULT at +180, and
  returns a NULL sound without printing anything** — hence a clip that is "ready" with length 0 and
  no error in the log. The AudioManager is context manager 3 (`mov r0,#3; bl 0xdfa44` right before
  the only call); libunity has a getter stub at `+0xd2454` and its own FMOD_ErrorString at
  `+0xce898` (a 0–95 jump table). Next (A12): the tracer reads +180 on every zero-length `Play()`
  and prints FMOD's own message — build-checked word by word before calling anything.
- **A12 — ROOT CAUSE (proven statically, device confirmation next).** The probe armed (all four
  words matched) and on both menu-music `Play()` calls the AudioManager held
  **`FMOD_RESULT=33` = `FMOD_ERR_INTERNAL`** ("An error occured that wasn't supposed to"). FMOD's OS
  thread creator (`+0x485828`, the only FMOD `pthread_create` site) does `pthread_attr_init →
  setdetachstate → setstacksize(max(req, 8192)) → setschedpolicy → setschedparam → pthread_create →
  attr_destroy`, and **every failure branches to `mov r0, #33`**. The FMOD file thread
  (`+0x47d760`) requests an **8192-byte stack**; bionic accepts it, but glibc's
  `pthread_attr_setstacksize` rejects anything below `PTHREAD_STACK_MIN` = **16384** (PalmPDK
  headers) with EINVAL, and apkenv's wrapper forwarded the size unchanged. So the file thread can
  never be created → every createSound that needs async file I/O (the MPEG music, `MPEGSEARCH`/
  streams) fails with 33 → a clip that is "ready" with length 0 → `Play()` starts nothing. Effects
  load as plain samples and never need that thread. This also explains why only 3 `pthread_create`
  calls ever reached apkenv's hook: the file thread fails *before* `pthread_create`. **Fix
  (systemic ABI translation):** when a bionic caller asks for less than glibc's minimum, clamp up to
  `PTHREAD_STACK_MIN` — what bionic would have granted. Opt-in `APKENV_PTHREAD_STACK_CLAMP=1`, on in
  `packaging/aralon/apkenv.env`, so Temple Run 2 stays byte-for-byte as shipped.
- **A13 (`logs/aralon-a13.log`) — the clamp works, the floor was too low.** `[PTHREAD]
  setstacksize(8192) below glibc's minimum -> 16384` fired, FMOD then reached `pthread_create` for
  the file thread (routine `+0x485994`) — i.e. the attr calls no longer fail — and the log ends there:
  the new thread never printed apkenv's trampoline line `[PTHREAD] >>> start`, no crash dump, process
  gone, no kernel fault line in syslog (webOS may not log user faults). **Refuted by measurement:**
  "static TLS eats the 16 KB" — the host Mono has no `PT_TLS` and no TLS symbols (pthread keys only).
  Working explanation (unproven, same knob): a glibc stack also holds the thread descriptor, and the
  trampoline's `fprintf` to unbuffered stderr makes glibc put an 8 KB buffer on the stack
  (`buffered_vfprintf`); FMOD sized 8 KB for bionic, where the stack holds only its own frames. The
  FMOD stream thread starts fine through the same trampoline with a larger stack. Fix: clamp small
  requests to a 128 KB floor (still tiny vs glibc's 8 MB default, lazily committed).
- **A14 (`logs/aralon-a14-live.log`) — MUSIC FIXED (user: "at the menu, music is playing!").**
  With the 128 KB floor: `[PTHREAD] setstacksize(49152) … -> 131072` (FMOD stream thread) and
  `setstacksize(8192) … -> 131072` (FMOD file thread); both `>>> start` lines appear; the thread
  sampler shows **`FMOD file thread`** (never before on the TouchPad, always on Android);
  `AudioSource.Play(src=…) clip=… len=219.1s ready=1 -> isPlaying now=1` — 219 s is the
  `sharedassets1.assets.resS` MP3 — and the meter reads a steady 5.5k–13.8k instead of `0`.
  **Root cause, in one line:** FMOD sizes thread stacks for bionic (8 KB for its file thread); glibc
  rejects anything under 16 KB (EINVAL, which FMOD turns into `FMOD_ERR_INTERNAL`), and even a
  16 KB glibc stack is too small once glibc's descriptor and apkenv's trampoline logging share it.
  Unity swallowed the createSound failure (AudioManager+156/+180) and kept a "ready" 0-length clip.
  Fix: `APKENV_PTHREAD_STACK_CLAMP=1` (in `packaging/aralon/apkenv.env`), 128 KB floor.
  Tools that found it: `APKENV_UNITY_ICALL_TRACE` (compat/icall_trace.c) + the Android reference.
  User also confirmed **zone music outdoors** ("i'm outside and music is playing there too!").
- **GRAPHICS — the terrain is not drawn (user screenshot `screencaptures/aralon_*103620.png` vs the
  Android reference at the same spot).** On the TouchPad the ground is solid **black** where the
  Mali tablet draws textured dirt and grass; cliffs, fences, NPCs, player, HUD and grass tufts all
  draw (the tufts with cyan/magenta fringe speckles). Black = the clear colour, so the likelier
  reading is *terrain not drawn*, not drawn black. Ruled out: shader failures — the GLES2 compile and
  link wrappers print `[GLSL] … FAILED` with the driver's log, and A14 has none. The game logs
  `Terrain Node Count: 10` (terrain is built). Android generated three fixed-function-emulation
  shaders (lighting/fog, logged `D`) on entering the world; the TouchPad never did — consistent with
  the terrain material never being drawn. GPUs differ: Android = ARM Mali (MediaTek), TouchPad =
  Adreno 220; Unity may pick different paths. **Instrument:** `APKENV_GL_UPLOADCHECK=1`
  (`compat/gl_uploadcheck.c`) checks `glGetError` around every texture/buffer upload and prints each
  rejected call (target, level, format, type, size, error) plus a format census. It hooks BOTH wrapper
  tables — the upload names are shared GLES1/GLES2 names and `register_hooks_nodup()` keeps the GLES1
  wrappers for them — and hands drained errors back on the engine's next `glGetError`, so the
  engine's error view is unchanged.
- **A15 (`logs/aralon-a15-live.log`) — no upload is rejected.** `APKENV_GL_UPLOADCHECK=1`, user
  standing at the black-ground spot after `Transition to zone: ContSouthStart`: **0 failures in 6000
  uploads**. Census: ETC1 ×598 (11.6 MB, CPU-decoded to RGB), RGBA4444 ×324, RGBA8888 ×45, ALPHA ×11,
  RGB888 ×8, RGB565 ×5, RGBA5551 ×1; 575 static buffers (0x88E4) and 4432 dynamic vertex-buffer
  updates (31 MB). So the terrain's data reaches the GPU; the fault is at draw time (not drawn, or
  drawn black). Next: the game's own terrain path (`Terrain Node Count: 10` is a game log line) and
  the `TerrainDetail` option.
- **Static, after A15 — the ground is the game's own meshes, not Unity Terrain.** No game method
  calls any Unity terrain API (`drawHeightmap`, `heightmapPixelError`, …). `TerrainPack:PostLoadGame`
  reads `optionTerrainDetail`, loads a `QuadTreeStream` from `Resources/terrain/`, and builds
  `terrainPackNode` meshes (`QuadNode.fillMesh`); detail fades in via **vertex colours**
  (`onUpdateFaders` → `QuadNode.addAlphaAll` → `set_colors`); `DynamicTerrain` enables/disables
  `TerrainNode` renderers by camera distance and far plane. **The game never branches on the GPU**
  (its only device queries are `Application.platform` and iOS `generation`). So the Adreno/Mali split
  must be in Unity's rendering: Android generated its fixed-function-emulation shaders (lighting,
  fog, spot lights) on entering the world and the TouchPad never did — i.e. Unity never draws the
  terrain's fixed-function material here. Unity silently skips an object whose shader has no
  SubShader it considers supported. Next: read the terrain material's compiled shader text out of the
  asset data (its SubShaders and their requirements) and Unity's caps decision for it.
- **Correction + new hypothesis (user at the device): the ground is NOT black, it is darker than on
  Android** — the webOS screenshot overstated it ("screenshots lie"). So the terrain *is* drawn, and
  the "never drawn" reading above is withdrawn. New hypothesis: our EGL surface is RGBA8888
  (`rgba=8/8/8/8`), Android's compositor treats a game surface as opaque, but the TouchPad's 3-layer
  compositor (`webos://knowledge/pdk`: wallpaper/launcher layer *under* the app layer) composites
  the GL layer with its alpha — and Aralon's ground fades detail in through vertex-colour alpha
  (`QuadNode.addAlphaAll` → `set_colors`), leaving alpha < 1 in the framebuffer. Result on screen:
  darker ground; in the capture tool: black; at non-premultiplied edges: the cyan/magenta speckles.
  The knowledge base confirms the layering but not the exact alpha rule — the device decides. Fix
  under test (A16): `APKENV_OPAQUE_PRESENT=1` → `apkenv_gles_opaque_alpha()` before
  `SDL_GL_SwapBuffers()` writes alpha = 1 across the frame (colour mask alpha-only), restoring
  scissor/clear-colour/colour-mask state; the game's colours are untouched.
- **A16 — alpha hypothesis REFUTED.** `[PRESENT] opaque present on` confirmed active; user at the same
  spot: "its no different". `APKENV_OPAQUE_PRESENT` stays opt-in and OFF for Aralon.
  **User's diagnosis (decisive observation):** on Android the house to the left of the shot casts a
  warm yellow light onto the area; on the TouchPad the house's windows are lit but **no light is cast**.
  → real-time **lighting** is missing. This matches the log evidence already in hand: on entering the
  world Android's Unity generated three fixed-function-emulation shaders with `computeSpotLight`,
  point-light attenuation (`light.atten`, `distSqr`), `unity_LightColor/unity_LightPosition` and fog —
  i.e. the per-vertex lighting shaders that apply those lights — and the TouchPad never generates
  them. Prime suspect: a GL limit Unity sizes vertex lighting from (`GL_MAX_VERTEX_UNIFORM_VECTORS` and
  friends) answered wrongly by apkenv — the shared `glGetIntegerv` name binds to the GLES1 wrapper.
  **Refinement (user):** there IS a slight glow on the TouchPad — "definitely less than android
  though — less color". So lighting is weaker/less coloured, not absent: consistent with Unity using a
  *different lighting variant* here (e.g. baked light only) than the Mali tablet's per-vertex lights.
  **Suspect refuted statically:** every `glGetIntegerv/glGetFloatv/glGetBooleanv` wrapper (GLES1 and
  GLES2) is a plain pass-through, and the GLES1 ones are re-pointed to libGLESv2 once the ES2 context
  exists — Unity gets the Adreno's real limits. Next: whether libunity keys lighting on `GL_RENDERER`
  (an Adreno workaround would make this Unity's own choice for the GPU, not a shim bug).
- **GRAPHICS PARKED (user decision, 2026-09-14):** "we shouldn't fuck with it. maybe its just the
  natural brightness of the touchpad screen vs the android device screen." The difference is subtle
  (a slight warm glow is present, just less than on the Mali tablet). Last static check: libunity has
  **no GPU-name strings** (`Adreno`/`Qualcomm`/`Mali`/`PowerVR`/`Tegra`), so no renderer-keyed Unity
  workaround to find; it does carry the `VERTEXLIGHT_ON` keyword. **Open lead if it is ever revisited:**
  Android generates fixed-function vertex-light shaders (`computeSpotLight`, `unity_LightColor`) on
  entering the world and the TouchPad does not — compare the lighting variant Unity picks per GPU
  (e.g. log `glShaderSource` for `unity_LightColor` / `VERTEXLIGHT_ON` programs). Diagnostics left in
  the tree, both opt-in and OFF for Aralon: `APKENV_GL_UPLOADCHECK` (0 upload failures in 6000) and
  `APKENV_OPAQUE_PRESENT` (no visible effect).
- **Package 0.2.0 built and ship-checked (not yet installed):**
  `apkenv/packaging/out/com.apkenv.aralon_0.2.0_all.ipk` (291.5 MB). The env inside the ipk is exactly
  `APKENV_HOST_MONO`, `APKENV_UNITY_PORTRAIT=0`, `APKENV_UNITY_SPLASH=0`, `APKENV_UNITY_PACKAGE`,
  `APKENV_UNITY_OBB`, `APKENV_PTHREAD_STACK_CLAMP=1` — no debug/instrumentation vars; `appinfo.json`
  0.2.0 (bumped because `palm-install` silently refuses a same-or-lower version); the binary in the
  ipk matches the local build (md5 `b8cd9f23b366`). The tracers it carries (`APKENV_UNITY_ICALL_TRACE`,
  `APKENV_GL_UPLOADCHECK`, `APKENV_OPAQUE_PRESENT`, `APKENV_SYS_CPU`) are all opt-in and dormant.
- **RELEASED 1.0.0 (2026-09-14) — fresh-install verified.** `com.apkenv.aralon_1.0.0_all.ipk`
  (291,501,188 bytes). Ship check: env exactly the six release lines, version 1.0.0, binary md5
  `b8cd9f23b366` (byte-identical to the A14 build the user heard music with). Fresh install: old app
  removed (`palm-install -r`, dir gone), the user's data moved aside to
  `/media/internal/.apkenv/aralon.apk.bak-20260914-110225` (restorable), test debris removed, 1.0.0
  installed, installed binary + env verified on the device. First launch (`logs/aralon-1.0.0-fresh.log`):
  packaged launch, `no store yet … (fresh profile)`, unity4 host, OBB registered, both FMOD stack
  clamps fired at the menu, **0 debug-tracer lines**, still running at +90 s. The user confirmed by
  ear: music plays at the main menu of the fresh install.
- Process: the 291 MB `.ipk` takes ~5 min to copy + ~3 min for the device to gunzip the OBB at
  ~1 MB/s; `tools/tr2-run.sh`'s 600 s install timeout would have launched a half-installed app
  (now `INSTALL_TIMEOUT`, default 1800). For binary-only iterations, `novacom put` the new
  `apkenv` straight into `/media/cryptofs/apps/usr/palm/applications/com.apkenv.aralon/` and
  `palm-launch` — the package only needs rebuilding when the env/assets change.
