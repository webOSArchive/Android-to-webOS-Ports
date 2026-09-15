# Fruit Ninja (Halfbrick **Mortar** engine) — port trail

Target: **`fruitninja_1.8.8.apk`** (`com.halfbrick.fruitninjafree`, versionCode 18080, built
2013-10-02). Started 2026-09-15 following `PORTING-PLAYBOOK.md`.

## 0. Which apk (decision)

Two candidates were on disk:

| | `fruitninja_1.8.8.apk` | `04 Fruit Ninja 2.1.2 (201201)/` |
|---|---|---|
| package | `com.halfbrick.fruitninjafree` | `com.halfbrick.fruitninja` |
| data | **all 87 MB inside the apk** (`assets/Standard/`) | 56 MB **OBB** + apk |
| engine | `lib/armeabi/libmortargame.so` (6.9 MB) | same name, newer |
| target SDK | 10 | 19 |

**Chose 1.8.8**: self-contained (no OBB ⇒ no 5-minute install cycles), older host generation,
same engine family so the work carries to 2.1.2 later. Both are landscape, GLES2, `minSdk 10`,
launch activity `com.halfbrick.mortar.MortarGameActivity`.

## 1. Triage

- **Engine:** Halfbrick **Mortar**, `lib/armeabi/libmortargame.so`. Not a proxy — 6.9 MB, and
  the JNI entry points and asset path strings (`assets/Standard`, `assets/PlatformGoogleFree`)
  are all in it. A second lib, `libMicroMapJNI.so`, is a map SDK; its `DT_NEEDED` set is plain
  bionic, so it loads harmlessly.
- **GL:** `DT_NEEDED` is **`libGLESv2.so` only**; assets carry `shaders/gles2/*.vs|.fs`.
  ⇒ pure ES2, no fixed-function path to worry about, no dual-table `register_hooks_nodup`
  hazard.
- **Orientation:** manifest declares `android.hardware.screen.landscape`; device is landscape.
  **No FBO rotation needed** (unlike WMW / TR2).
- **Textures:** custom `.tex` = 12-byte header (`log2w, log2h, 1, 0, u16 w, u16 h`) + **raw
  RGBA8888**. Verified: `32x32` ⇒ 32·32·4 + 12 = 4108 bytes. **No ETC1**, so `compat/etc1.c`
  is not in play. 63 MB of textures, uncompressed.
- **Audio:** 220 `.ogg` (Vorbis 44100 Hz). The engine decodes Vorbis itself (vorbis strings in
  the `.so`, and `AudioDecoderStream_native_*` exports are a decoder *Java* calls). Output is
  §3 below.
- **Assets:** read by the engine's **own zip reader** straight out of the apk (`fopen`/`open` +
  a statically linked inflate; string `Zip archive inconsistent`). No custom container, no
  Derbh/DTRZ-style unpack step — the §8 "unpack in advance" trap does not apply. 302 entries
  Stored / 1063 Deflated under `assets/Standard/`.
- **Java share:** large but almost entirely ads/social/billing (MoPub, AdColony, Vungle, Zigi,
  Beintoo, OpenFeint, GCM, IAB v3). The *game* is entirely native.

## 2. Host contract (derived statically, no device)

Intersecting the method names defined in the game's own Java packages (`com/halfbrick/**`) with the
engine binary's string table gives the names the engine can call into Java; resolving each to its
class, signature and modifiers gives `plan/fruitninja-contract.txt` — 189 rows, regenerate with
`apkenv/tools/fn-contract.sh`. Scope the name scan to the vendor packages: the apk also bundles
MoPub, AdColony, Vungle and Google Play, whose generic method names (`init`, `start`, `read`,
`close`, `values`) collide with unrelated strings and bury the real contract in 900 rows of noise.

### 2a. Engine entry points (`nm -D` on `libmortargame.so`)

The 1.7.x generation the *upstream* `modules/fruitninja.c` targets is **gone**: there is no
`native_init` and no `native_InitFileManager(String,String,Z)`. 1.8.8 splits init in two and
`InitFileManager` grew a third path:

```
native_SystemInit(II Ljava/lang/String;)V         (width, height, Locale.getLanguage())
native_GameInit()V
native_InitFileManager(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V
                                                  (apk sourceDir, filesDir, cacheDir, false)
native_InitJavaSoundManager()V
native_InitOpenSLSoundManager(Landroid/content/res/AssetManager;)Z
native_step()Z                                    false => shut down
native_touchEvent(IJIFFFF)V   (action, eventTime, pointerId, x, y, pressure, size)
native_keyEvent(IZZ)V         (keycode, isDown, cancelled)
native_onPause/onResume/onFocusLost/onFocusRetrieved/saveOnExit()V
native_gameRequestedQuit()Z   native_gameRequestedRestart()Z
native_DrawSplashQuad(I)V     native_GLESVersion()I    native_SetAppLicensed(Z)V
```

**Touch coordinates are NORMALIZED 0..1** — `MultiTouchInputHandler.SendIndividualTouchEvent`
does `getX(i) / display.getWidth()` and `getY(i) / display.getHeight()` (`div-float` confirmed
in the smali). Same convention as WMW; feeding pixels would look exactly like "touch is dead".

### 2b. Boot order (`GameManager` + `MortarGameView$Renderer`)

GLSurfaceView calls `onSurfaceChanged` (sets `mWidth/mHeight`) before the first `onDrawFrame`,
then frame 1 is:

```
GameManager.SystemInit(gl):
    NativeGameLib.GLESVersion()                 // ==1 => disable client states (ES1 only)
    glFrontFace(GL_CW); glDisable(GL_CULL_FACE); glDisable(GL_DITHER)
    initFileSystem(ctx):
        InitFileManager(apkPath, filesDir+"/", cacheDir+"/", false)
        if (!InitOpenSLSoundManager(assets)) { UsingOpenSL=false; InitJavaSoundManager(); }
    SystemInit(mWidth, mHeight, Locale.getLanguage())
    doLicenseCheck()                             // EMPTY in the free build
if (mSplash == null) { GameInit(); postGameInit(); }
```

and every frame after that:

```
GameManager.Render(gl):
    glFrontFace(GL_CW); glEnable(GL_DITHER); glCullFace(GL_BACK); glEnable(GL_DEPTH_TEST)
    drain queued KeyEvents -> keyEvent(code, isDown, cancelled)
    if (!step()) shutdownApp()
    if (gameRequestedQuit())    -> quit dialog
    if (gameRequestedRestart()) -> restartApp()
```

The module reproduces exactly this, taking the **no-splash** branch (the Java splash is a
`SplashScreenTimed` that uploads a res drawable and calls `native_DrawSplashQuad`; skipping it
is the same path a device with the splash disabled takes). *The shim still owes the player a
window over the load* — see §5.

### 2c. The audio contract (the one that decides whether there is sound)

`NativeGameLib.SupportsOpenSL()` returns **true on everything except Lenovo**, so on a real
phone Fruit Ninja uses **OpenSL ES**. webOS has no `libOpenSLES.so`, and the module answers
`SupportsOpenSL -> 0`, which sends the engine down its fallback: the engine mixes everything
itself and **pushes** PCM at Java through

```
com/halfbrick/mortar/MortarAudioMixerOut.Create()Lcom/halfbrick/mortar/MortarAudioMixerOut;
                                        .WriteData([B)V
                                        .WriteData([S)V
```

`MortarAudioMixerOut`'s constructor is `new AudioTrack(STREAM_MUSIC, 44100, CHANNEL_OUT_STEREO,
ENCODING_PCM_16BIT, getMinBufferSize(...), MODE_STREAM)` — i.e. **44100 Hz stereo S16, push
model**. That is exactly `audio/audiotrack.c` (`apkenv_audiotrack_create` + blocking
`apkenv_audiotrack_write`), which already implements AudioTrack's back-pressure. The engine
does its own mixing on its own "Audio Thread" (`pthread_create`, 6 `pthread_attr_*` imports, no
`setstacksize` — so the Aralon 8 KB-stack trap does not apply).

### 2d. Engine callbacks that *are* native (RegisterNatives)

31 of the contract rows are declared `native` in the smali and are bound by the engine's
own `RegisterNatives` (the `JNINativeMethod` tables sit in `.data.rel.ro`; verified by locating
`{"native_threadEntry", "(I)V", 0x4d8348}` at file offset `0x69d90c`). These are **callbacks,
not host services** — when the host must answer one (e.g. dismissing a dialog), the module
looks the registered function up with `jnienv_find_native_method()` and calls the engine's own
implementation. Notable members: `MortarDialog.ButtonWasPressedNative(II)`,
`NativeGameLib.native_keyboardProcess*`, `Provider_*.{BannerAdLoadResult,FullscreenAd*}`,
`MortarPurchaseObserver.PurchaseResultNative`.

**`native_threadEntry` looked inert from the static pass and was not.** The only reference to the
string is that registration table and no Java caller exists, so the first read of this file said
"dead code from an older build". In fact the engine creates a worker with `pthread_create`, and the
trampoline calls this Java static so the thread is attached to the VM — and the Java static *is* one
of the registered natives, so the call lands back in the engine. That thread builds
`MortarAudioMixerOut` and pumps the mix. Dropping it silently cost the game all of its sound. The
tracer is what caught it (`UNHANDLED void native_threadEntry(I)V`, one line before the thread's
`<<< end`); see §5.

**And none of these are bound until `JNI_OnLoad` runs.** `libmortargame.so` exports it and that is
where all 32 `RegisterNatives` calls happen. The module calls it first thing in `init()`.

### 2e. Host services the module implements

| Java | answer | why |
|---|---|---|
| `NativeGameLib.SupportsOpenSL()Z` | `0` | no OpenSL on webOS ⇒ take the Java mixer path (§2c) |
| `MortarAudioMixerOut.Create()` / `WriteData([B/[S)` | AudioTrack ring | the audio path |
| `HBSupport.GetDensityDPIType()I` | `160` | TouchPad 1024×768 @ 9.7" ≈ 132 dpi ⇒ mdpi |
| `HBSupport.IsDeviceTablet()I` | `1` | Java computes diagonal ≥ 6.5" ⇒ tablet; 9.7" is one |
| `HBSupport.GetPhysicalScreenSizeTypeMask()I` | `0x14` | `SCREENLAYOUT_SIZE_XLARGE|LONG_NO` |
| `HBSupport.GetTouchscreenCapabilities()I` | `2` | Java: -1 none / 0 touch / 1 multitouch / **2 distinct** |
| `HBSupport.GetWifi()I`, `Reachability.isOnline()Z`, `Beintoo.IsOnline()Z` | `0` | offline device |
| `HBSupport.{Get,Set}BoolPreference`, `PreferenceKeyExists` | `prefs.txt` in the data dir | Android `SharedPreferences("MortarGameActivity")` |
| `HBSupport.Get{PackageName,PackageVersion,Model,Manufacturer,AndroidVersion,Country,DeviceLanguage,DeviceLocale,UUID,AndroidID,DeviceID,GoogleAccount,AccountEmails}` | fixed strings | device identity |
| `HBSupport.Init(key,iv)` + `Encrypt([B)[B` / `Decrypt` | **AES-128-CBC, NoPadding** | Java uses `javax.crypto` with the key/iv `Init` was given; identity would break any round-trip the engine does not own both ends of |
| `KeyStore.GetValue/SetValue/SetValueIf` | `KeyStore.dat` in the data dir | Java stores it under a `Halfbrick` folder on external storage |
| `MortarDialog.createDialog/showDialog/removeDialog` | `true`, then **answer** via `ButtonWasPressedNative` | Java's `createDialog` is async (`runOnUiThread`) and returns true; the engine waits for the callback. This is the PvZ `getInputString` shape — the #1 freeze candidate |
| `SoftKeyboard.UpdateKeyboard()Z` | `0` | Java returns `IsSoftInputActive()`; no keyboard is up |
| `MortarGameActivity.SetScreenResolution(II)` | log | native tells the host its resolution |
| everything else (ads, billing, Beintoo, Zigi, WebView, Youtube, push, HTTP) | `0` / `NULL` / no-op + **tracer** | absent services; `isOnline=false` keeps the engine from waiting on them |

`doLicenseCheck()` is **empty** in the free build, so `native_SetAppLicensed` is *not* called
(the upstream module called it unconditionally — that would be unfaithful here). Knob:
`APKENV_MORTAR_LICENSED=1`.

## 3. Instrumentation (before any device test)

`modules/mortar.c` ships an always-on unhandled-call tracer (`mortar_trace_unhandled`, the
`marmalade.c` template): each unimplemented engine→host call is printed once with its
signature, then at 100 / 10k / 1M. Per-contract log lines: `[MORTAR]` for boot steps,
`[MORTAR-AUDIO]`, `[MORTAR-DIALOG]`, `[MORTAR-PREF]`, `[MORTAR-KS]`, `[MORTAR-TOUCH]`
(first 20 events, raw → normalized).

## 4. Device trail (2026-09-15)

One package install, then binary-only pushes (`apkenv/tools/push-run.sh`). Logs in `plan/logs/`,
screenshots (gitignored) via `apkenv/tools/grab.sh`.

| run | change | result |
|---|---|---|
| fn-01 | first launch of the new module | **Booted on the first try.** ES2 shader renderer, GL_RENDERER=Adreno 220, full-screen 1024×768, correct aspect, textures right. `grab.sh` showed the first-run trailer screen. Two problems: the engine polled `HttpClient.IsFinished` 2845 times in 45 s (GetFieldID returning 0), and a worker thread called `native_threadEntry` and exited immediately. |
| fn-02 | real `HttpClient` object + fields; `native_threadEntry` dispatch | Polling storm gone. **SIGSEGV** in the engine's NetworkManager with a NULL pointer, right after the second request. `native_threadEntry` → "NOT REGISTERED". |
| fn-03 | HTTP fails asynchronously (30 frames later) + `RequestPointer` set | No crash; the game runs indefinitely through its ad-media download loop. `native_threadEntry` still unregistered, and `RegisterNatives` was never called at all. |
| fn-04 | **call `JNI_OnLoad`** | 32 engine callbacks registered, `native_threadEntry` dispatched, and the engine immediately did `MortarAudioMixerOut.Create` and started pushing PCM. **Audio.** |
| fn-05 | `APKENV_MORTAR_AUTOTAP` (synthetic tap) | Tap at (890,660) → normalized (0.869,0.859) → trailer screen dismissed, **main menu**. Touch contract proven without a finger. |
| fn-06 | autotap extended to swipes | A slice through the New Game watermelon → **Mode Select**. |
| fn-07 | slice into Classic | **Gameplay**: timer counting down, pause button, fruit thrown. |
| fn-08 | audio meter every 10 s | 50 s of audio in a 55 s run, **underrun +0** after the initial prefill — the pump tracks real time exactly. |
| fn-09 | 21 slicing swipes across the play area | **Score 4**, two strikes, a bomb exploding with particles. Slicing, scoring and hazards all work. |
| fn-10 | fresh `.ipk` install of the shipping build | Launches from the icon straight to the **main menu** (the first-run flag persisted across the reinstall, so saving works), shipped env has no debug vars, audio steady, no crash. |

## 5. What was wrong, and what generalizes

**Call `JNI_OnLoad`.** This was the one that mattered. `libmortargame.so` exports it (0x239fe0), and it
is where the engine `RegisterNatives` **32 of its own callbacks** — `native_threadEntry`,
`MortarDialog.ButtonWasPressedNative`, the keyboard callbacks, every ad/billing result native.
Without it the engine's worker thread called `native_threadEntry`, got nothing, and exited one log
line later; that thread is the one that builds `MortarAudioMixerOut` and pushes PCM, so the whole
game was silent. Nothing in the contract table predicted this: the static scan found exactly one
reference to the `native_threadEntry` string (its `JNINativeMethod` entry in `.data.rel.ro`) and I
read that as "dead code from an older build". **The tracer caught what the static pass got wrong** —
which is the argument for shipping the tracer before the first device run.

**An asynchronous host operation must stay asynchronous.** Answering `HttpRequest` *inside the call*
— IsFinished already true when it returned — made the engine parse a response for a request it had
not finished registering, and it dereferenced NULL in its NetworkManager. The Java host always
finishes on another thread; retiring the request ~30 frames later (and setting `RequestPointer`,
which the Java worker sets and the engine uses to find its own request context) fixed it. Reproduce
the host's *timing*, not just its values.

**Reproduce the failure path the Java host actually has.** `HttpClient$1`'s two catch blocks are
precise: `Result = new byte[0]`, `IsFinished = true`, `ResponseCode` left at 0, `ReturnedHeaders`
left **null**. That is what an offline device produces, and the game already has a screen for it.

**Read the button numbering, don't guess it.** `MortarDialog$3`'s listeners give
positive→`ButtonWasPressed(id,1)`, negative→`(id,0)`, cancel→`confirmQuitRequest(false)`. The one
dialog identifiable in this build is the quit confirmation, so **0 is the safe auto-answer** and 1
would quit the game by itself.

**Synthetic input closes the loop when nobody is holding the device.**
`APKENV_MORTAR_AUTOTAP="x,y@frame;x1,y1>x2,y2@frame"` presses, drags and releases through the real
`module->input` path, so it exercises the actual contract (normalized 0..1, Android action codes);
paired with `tools/grab.sh` it took the port from "boots" to "scored 4 points in Classic mode"
without a person in the room. Diagnostic only — never in a shipped env file.

## 6. Status

- [x] Triage, contract table (`plan/fruitninja-contract.txt`, regenerate with `apkenv/tools/fn-contract.sh`)
- [x] `modules/mortar.c` + tracer + build wiring
- [x] Boots, renders full-screen landscape on its own ES2 device
- [x] Audio: engine mixer → `MortarAudioMixerOut` → `audio/audiotrack.c`, 44100/stereo, no underruns
- [x] Menus, mode select, Classic gameplay, slicing, scoring, bombs — verified by synthetic input
- [x] Saves persist (`KeyStore.dat`, prefs) across a reinstall
- [x] `packaging/fruitninja/` + `.ipk`, installs and launches from the icon, no debug env
- [ ] **Needs the operator** (see §8)

## 7. Whose job list (what the Android platform did that we now don't)

- **Splash / window over the load.** Java's `SplashScreenTimed` is skipped (the module takes the
  `mSplash == null` branch). The load is short enough that fn-01..fn-10 never showed a long black
  screen, but if it looks broken on a cold boot, supply it the TR2 way: `APKENV_SPLASH_RGB` from the
  game's own `res/drawable*` art, retired on the engine's first draw.
- **SharedPreferences / KeyStore.dat** — ours now, in the per-apk data dir.
- **Dialogs** — ours now; an unanswered one is a freeze.
- **Sound/music sliders** — the engine mixes internally, so its own settings screen drives them.
  Nothing for the shim to wire (unlike TR2).
- **Ads / billing / leaderboards / the YouTube trailer** — absent by design. `isOnline() = false`
  keeps the engine from waiting on them; it still issues one ad-media `GET` a second forever, each
  failing after ~1 s, which is what an offline Android device does too.

## 8. Open — needs a person with the device

1. **Real finger touch.** Every touch so far was synthetic. The path is the same one SDL feeds, and
   the package is launched from its icon (which is what makes SDL deliver input at all — playbook
   §4), but `ev_total=1` in all ten runs because nobody touched the panel. *Expected to work; not
   yet observed.*
2. **Does it actually sound right?** The pump is provably fed and drained in real time with zero
   underruns, which rules out silence-by-starvation — but audible, correct-pitch music and SFX are
   an ears question.
3. **Multi-finger slicing.** The module maps a second finger to
   `ACTION_POINTER_DOWN | index<<8`, faithfully to `MultiTouchInputHandler`, but only single-finger
   input has been exercised.
4. **Feel and frame rate.** ~45 fps by the SDL heartbeat over 45 s; whether it *plays* well is a
   judgement call.
5. **The other modes** (Arcade, Zen, Dojo, Extras) and a full Classic game to the results screen.

## 9. Test protocol (one change per run)

Full package: `APPID=com.apkenv.fruitninja WAIT=50 apkenv/tools/tr2-run.sh fn-NN`
Binary only:  `APPID=com.apkenv.fruitninja WAIT=50 apkenv/tools/push-run.sh fn-NN`
Screenshot:   `apkenv/tools/grab.sh fn-NN` (never the on-device screenshot)

Healthy log, in order:

```
apkenv: packaged launch; apk = .../android/fruitninja.apk
[MORTAR] try_init: found 17/17 natives (Mortar 1.8.x generation)
[MORTAR] JNI_OnLoad
[MORTAR-REG] com/halfbrick/mortar/NativeGameLib.native_threadEntry(I)V -> 0x...
[MORTAR] InitFileManager(...)
[MORTAR] SystemInit(1024, 768, "en")
[MORTAR] SupportsOpenSL -> 0 (webOS has no OpenSL; using the Java mixer path)
[MORTAR-AUDIO] MortarAudioMixerOut.Create -> AudioTrack 44100/2 (open)
[MORTAR] GameInit
[MORTAR] step #1 -> 1
[MORTAR-AUDIO] 221 writes, 1771536 bytes (10s of audio), underrun +0
```

| symptom | first thing to read |
|---|---|
| no `step #1` | the last `[MORTAR]` line names the boot step that hung |
| runs, black screen | `tools/grab.sh fn` |
| freeze after a few seconds | `[MORTAR-JNI] UNHANDLED …` — a blocking host call; `MortarDialog` first |
| silent | `[MORTAR-AUDIO]`: no `Create` ⇒ `JNI_OnLoad`/`native_threadEntry` broke again; `Create` but the byte count stops growing ⇒ the engine's mixer thread died; `underrun +N` climbing ⇒ the pump is starving |
| taps do nothing | `[MORTAR-TOUCH]` raw → normalized; both must be 0..1 |
