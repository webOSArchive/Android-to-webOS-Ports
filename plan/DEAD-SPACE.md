# Dead Space (EA Mobile, BLAST engine) — port plan

Target: **`android-candidates/Dead-Space.apk`** — `com.eamobile.deadspace_full_azn` 1.2.0
(versionCode 1200), the **Amazon Appstore** build, dated 2013-10-22. 296 MB.

Status: **boots and renders frame 1; crashes on frame 2.** See §6 for the device trail.
Previously: **module written, package built, nothing run yet.** 2026-09-15 evening, following
`PORTING-PLAYBOOK.md` §1–§2. Everything below is static analysis —
`apkenv/modules/eablast.c` compiles and
`apkenv/packaging/out/com.apkenv.deadspace_1.0.0_all.ipk` (174 MB) is ready to install, but **no
part of this has executed on a device.** Treat every claim as a derivation, not an observation.

## 1. Triage

| | |
|---|---|
| Engine | **EA BLAST** (`com.ea.blast.*`) + EAIO / EAThread / EAAudioCore / rwfilesystem (RenderWare lineage). One native lib, `lib/armeabi/libDeadSpace.so`, 6.6 MB — **`armeabi` only, no v7a**. |
| GL | **GLES1 fixed-function.** 190 `gl*` imports, all ES1 + OES extensions (`glAlphaFunc`, `glColor4f`, `glColorPointer`, `glClientActiveTexture`, `glClipPlanef`, `glBindFramebufferOES`…). **Zero** ES2 shader calls, **zero** `eglGetProcAddress`. `DT_NEEDED` is `libGLESv1_CM.so` alone. |
| Orientation | No `screenOrientation` in the manifest — the engine decides (`DisplayAndroidDelegate.Get/SetStdOrientation`). Dead Space is landscape. |
| Audio | `AndroidEAAudioCore` → a real `android.media.AudioTrack` handed **to the native side**, which writes to it over JNI. |
| Data | `assets/published/` — **319 MB, already unpacked in the apk**, read with plain `fopen`/`open`/`mmap`. No OBB. |
| Java share | 637 classes, but ~90% is licensing/DRM/download scaffolding (Google licensing 194, Amazon 100+, EA download 109, Verizon 18). The engine host is **25 classes in `com/ea/blast`**. |

> **The manifest says `uses-gl-es 0x20000` and the binary is pure GLES1.** Trust the binary. This is
> good news: apkenv's ES1 path is the most mature one here (Where's My Water), there is no
> dual-table `register_hooks_nodup` hazard because only `libGLESv1_CM` is linked, and the
> fixed-function rotation hooks are available if orientation turns out to need them.

## 2. Host contract

50 names, in `plan/deadspace-contract.txt`. It is unusually clean — almost entirely
**value-returning device facts**, not blocking-capable calls:

- `SystemAndroidDelegate.Get*()Ljava/lang/String;` — ~25 getters: `GetTotalRAM`, `GetChipset`,
  `GetDeviceModel`, `GetApiLevel`, `GetLanguage`, `GetTouchScreenCount`, `GetAccelerometerCount`,
  … Everything returns a **String**, including the counts. Answer them all deliberately;
  `GetTotalRAM` in particular is likely to size caches on a game this big.
- `DisplayAndroidDelegate` — `GetDefaultWidth/Height`, `GetDpiX/Y`, `Get/SetStdOrientation`.
- `GetAppDataDirectoryDelegate` — `GetAppDataDirectory()` = `getFilesDir()`,
  `GetExternalStorageDirectory()` = external storage root (or `""` if not mounted).
- `TouchSurfaceAndroid.IsTouchScreenMultiTouch()Z`, `MainActivity.GetInstance()`,
  `LocationManagerAndroid.SetEnabled`, `VirtualKeyboardAndroidDelegate.Shutdown`,
  `AndroidEAAudioCore.Startup/Shutdown`.

### Engine entry points (`nm -D`)

```
MainActivity_NativeOnCreate / OnPause / OnResume / OnStop / OnWindowFocusChanged(Z)
             NativeOnOrientationChanged / OnLowMemory / OnScreenOff / GetExitCode()I / OsExit
AndroidRenderer_NativeOnSurfaceCreated / NativeOnSurfaceChanged / NativeOnDrawFrame
TouchSurfaceAndroid_NativeOnPointerEvent
                    NativeGetIdRawPointer{Down,Up,Move,Cancel} / NativeGetIdUndefined
KeyboardAndroid_NativeOnKeyDown / OnKeyUp / OnCharacter / OnVisibilityChanged
AccelerometerAndroidDelegate_NativeOnAcceleration
DeviceOrientationHandlerAndroidDelegate_NativeOnDeviceOrientationChange + 6 constant getters
DisplayAndroidDelegate_NativeGetOrientation{Normal,RotatedLeft,RotatedRight,UpsideDown,Unknown}
ModuleCatalog_NativeGetModuleTypeId{TouchScreen,Accelerometer,Display,…}
EAIO_Startup/Shutdown  rwfilesystem_Startup/Shutdown  EAThread_Init
AndroidEAAudioCore_Init(AudioTrack,III) / Release
DeadSpace_runEntryPoint
JNI_OnLoad / JNI_OnUnload
```

**The engine publishes its own constants.** `NativeGetIdRawPointerDown/Up/Move/Cancel` and the
orientation/module-type getters mean Java *asks the engine* what the enum values are, instead of
hardcoding Android's. So the module must not invent action codes — call the getters and use what
comes back. This is the opposite of Fruit Ninja, where the Java host translated and the engine took
Android's raw `MotionEvent` action.

### Boot order (`MainActivity.onCreate`)

```
static: System.loadLibrary("DeadSpace")      -> JNI_OnLoad
MainActivity.onCreate:
    super.onCreate
    SetCommonPreferences()
    instance = this
    NativeOnCreate()                          <- before any GL exists
    new AndroidView(...)                      <- the GLSurfaceView
    setContentView(FrameLayout(mGLView))
    register SCREEN_OFF receiver, PhysicalKeyboardAndroid, ...
DeadSpaceActivity.onCreateDeadSpaceActivity:  ... AndroidEAAudioCore.Startup()
then the renderer: NativeOnSurfaceCreated -> NativeOnSurfaceChanged(w,h) -> NativeOnDrawFrame ...
```

### Audio contract (read the ctor, do not guess the rate)

```java
Startup()            -> Startup(2)
Startup(int channels):
    rate   = AudioTrack.getNativeOutputSampleRate(STREAM_MUSIC)
    minBuf = AudioTrack.getMinBufferSize(rate, CHANNEL_OUT_STEREO, ENCODING_PCM_16BIT)
    track  = new AudioTrack(STREAM_MUSIC, rate, CHANNEL_OUT_STEREO, ENCODING_PCM_16BIT,
                            minBuf, MODE_STREAM)
    Init(track, rate, channels, minBuf)       // native keeps the object and writes to it
```

Same shape as Fruit Ninja's `MortarAudioMixerOut`, one level more indirect: the engine is handed a
real `AudioTrack` **object** and calls `write`/`play`/`flush`/`release` on it through JNI. So the
module answers `GetMethodID(AudioTrack, "write", "([SII)I")` and friends and routes them into
`audio/audiotrack.c`, then calls the exported `..._AndroidEAAudioCore_Init` with our object.
We choose the rate — 44100 for the TouchPad.

## 3. Where the data lives — the decision that shapes the package

The engine opens **relative paths** (`published/sounds/soundBase.sb`, `published/sounds/sfx/`) with
plain `fopen`/`open`/`mmap`. It has **no zip reader for assets** (no `unz*`/`inflate` imports) and
**no `AAssetManager`**. So, unlike Fruit Ninja, it cannot read its content out of the apk: the
`published/` tree has to exist as real files, rooted wherever the engine composes those paths from —
almost certainly `GetAppDataDirectory()` or `GetExternalStorageDirectory()`, which we answer.

`ContentUnzip` (the Java class that would normally stage this) looks for a **`data.zip`** and
unpacks it to `/sdcard/Android/data/com.ea.deadspace_full_azn/files/`, warning that *"150MB is
required"*. **This apk contains no `data.zip`** — it ships `assets/published/` pre-unpacked instead.
So that class is a legacy/other-SKU path, and staging is ours.

Two options, and the choice matters because 319 MB is not something to copy twice:

1. **Point the engine at the read-only app dir.** `published/` is content, not save data. If the
   base for `published/...` comes from a delegate we answer, answer it with the app's own
   `android/…` directory and copy nothing. Saves go to the writable data dir separately (the engine
   has two delegates for exactly this reason). **Try this first.**
2. Fall back to `DATA=` seeding (`android/<apk>.data/` → `/media/internal/.apkenv/<apk>/`, playbook
   §5) only if the engine insists on one writable root for both. That is a first-run copy of 319 MB
   file-by-file through `copy_tree` onto FAT — minutes, and it doubles the space used.

**Settled, by covering both cases instead of answering the question.** The xref hunt was a dead end:
the engine is PIC, so the string address is a PC-relative delta rather than a stored pointer, and a
delta scan over the whole binary returns 1188 candidate sites — too noisy to be worth refining
(`tools/` has no xref helper for this; the naive absolute-pointer search in the Fruit Ninja notes
only works for `.data.rel.ro` pointers). So the module does both:

- **`chdir()` into the content root** in `init()` — covers the engine using the paths bare.
- **answers `GetExternalStorageDirectory()` with that root's absolute path** — covers it prefixing.

`chdir` is safe at that point: the bionic libs (`APKENV_LOCAL_BIONIC_PATH="./libs/webos/"`) and the
engine `.so` are loaded before any module `init()` runs, and the packaged log was opened by absolute
path. Override the root with `APKENV_BLAST_CONTENT`.

### Packaging: strip the apk, ship the content, copy nothing

Bundling the apk whole *and* the extracted tree would be a ~615 MB package, over half of it dead
weight — and `DATA=` would additionally make apkenv seed 319 MB into `/media/internal` on first
launch. Neither is necessary, because the content only needs to be *readable*:

| | |
|---|---|
| `APK=` | **stripped apk**, 2.6 MB — `lib/` + `AndroidManifest.xml` + `resources.arsc` + `res/`. apkenv needs it only to find and load the engine `.so`, name the app, and let `build-ipk.sh` pull the launcher icon. |
| `EXTRAS=` | `assets/published/` as a real tree at `android/extras/published/`, **read in place**. `EXTRAS` (not `DATA`) precisely because it does no first-run seeding. |

Built by `apkenv/tools/ds-stage.sh`, which verifies the staged tree matches the apk's file count and
byte total exactly and that the engine `.so` survived the strip — a short content tree is a game
that boots and then cannot find a level, which is expensive to debug on the device. Result:
**174 MB `.ipk`** (gzip does well on the content), against ~615 MB for the naive approach.

## 4. Ranked risks

1. **Memory.** This is a full 3D console port on a ~940 MB device with ~490 MB free. 319 MB of
   content, and `GetTotalRAM` is a contract point the engine will believe. Set `requiredMemory` in
   `appinfo.json` honestly (PvZ needed ~450 MB and used it to make webOS reclaim before launch), and
   expect `NativeOnLowMemory` to matter. *This is the risk most likely to kill the port.*
2. **`pthread_attr_setstacksize` AND `pthread_attr_setstack`.** EAThread sets explicit stacks — the
   exact shape that silently killed Aralon's music (glibc rejects stacks under 16 KB; apkenv's
   answer is `APKENV_PTHREAD_STACK_CLAMP=1`). `setstack` (caller-allocated) is the more delicate of
   the two and may need handling beyond the clamp. **Turn the clamp on from the first run** and
   watch `/proc/self/task` for a thread the reference device has and we do not.
3. **`runEntryPoint` has no Java caller in this dex.** That is exactly what `native_threadEntry`
   looked like in Fruit Ninja this morning, and it was how the engine attached its audio thread.
   **Do not write it off.** Ship the tracer and let the device say.
4. **Compressed textures.** `glCompressedTexImage2D` plus `GL_NUM_COMPRESSED_TEXTURE_FORMATS` —
   the engine *asks the driver* which formats exist and adapts, which is better than Aralon's blind
   ETC1. The TouchPad's ES1 context advertises ATC. Verify with `APKENV_GL_UPLOADCHECK` rather than
   assuming `compat/etc1.c` is needed. Textures are real JSR-184 **M3G** files (magic
   `ab 4a 53 52 31 38 34 bb`) with an `MPP-M3G-TXC` compression section.
5. **Package size.** ~296 MB `.ipk`, i.e. Aralon territory: ~5 min USB copy plus several more
   unpacking. Use `INSTALL_TIMEOUT` and do binary-only iterations with `tools/push-run.sh`.
6. **`armeabi` only** (no v7a). Fine — the TouchPad is armv7 and runs armeabi — but it means no
   NEON-tuned path, so expect the slower of the two builds.

**Not a risk: the Amazon Kiwi DRM.** `Kiwi.onCreate/onStart/onResume/onPause/onStop/onDestroy` wrap
the *Activity* lifecycle, and we never run Java — the module drives the native side directly, so the
entitlement check never executes. The `kiwi` file and `com.amazon.content.id.*` markers are inert
for us.

## 5. Done so far, and the first device run

Written and building, none of it executed:

- `apkenv/modules/eablast.c` — named for the **engine**, since `com.ea.blast` is shared across EA's
  Android ports of this era. Boot order from the callers: `JNI_OnLoad` → `EAThread_Init` →
  `rwfilesystem_Startup` → `EAIO_Startup` → `NativeOnCreate` → `NativeOnSurfaceCreated` →
  `NativeOnSurfaceChanged(w,h)` → fetch the engine's pointer constants → audio → per-frame
  `NativeOnDrawFrame`. Always-on unhandled-call tracer, a log line per contract point, an audio
  meter and an fps meter.
- `apkenv/tools/ds-stage.sh`, `apkenv/packaging/deadspace/{appinfo.json,apkenv.env}`, and the
  174 MB `.ipk`.

### First run (a ~174 MB install, so make it count)

```
APPID=com.apkenv.deadspace WAIT=60 INSTALL_TIMEOUT=1800 apkenv/tools/tr2-run.sh ds-01
apkenv/tools/grab.sh ds-01
```

Expected, in order:

```
[BLAST] try_init: found 25/25 natives (EA BLAST host)
[BLAST] content root: .../android/extras (chdir ok)
[BLAST] JNI_OnLoad
[BLAST] EAThread_Init / rwfilesystem_Startup / EAIO_Startup
[BLAST] NativeOnCreate
[BLAST] NativeOnSurfaceCreated / NativeOnSurfaceChanged(1024, 768)
[BLAST] pointer ids: down=.. up=.. move=.. cancel=.. undefined=.. touchscreen module=..
[BLAST-AUDIO] AudioTrack 44100/2 open
[BLAST] first NativeOnDrawFrame returned
[BLAST-FPS] ... fps
```

| symptom | first thing to read |
|---|---|
| no `first NativeOnDrawFrame` | the last `[BLAST]` line names the boot step that hung |
| `chdir ok` missing | `APKENV_BLAST_CONTENT` is wrong; the engine will find no content |
| pointer ids all 0 | the constant getters did not resolve — check `[BLAST] try_init` found 25/25 |
| dies early, no clue | memory (risk 1) — check `requiredMemory`, and watch for `NativeOnLowMemory` |
| runs, silent | `[BLAST-AUDIO]`: no `write` traffic ⇒ suspect the pthread stack clamp / a missing thread |
| `[BLAST-JNI] UNHANDLED` | a contract gap; the tracer names it with its signature |

### Still to do

1. **`requiredMemory` is a guess (400).** Set it from a measured RSS after the first run that gets
   far enough to have one.
2. Copy the synthetic-input hook (`APKENV_MORTAR_AUTOTAP`, `modules/mortar.c`) across — it is
   engine-independent and it is what made Fruit Ninja testable without a person. Left out of the
   first build deliberately: it is worth nothing until the game draws a frame.
3. `runEntryPoint` is unwired and unexplained (risk 3). If the first run boots but nothing happens,
   this is the first suspect — it is the same shape as Fruit Ninja's `native_threadEntry`.
4. Orientation: the module assumes landscape and sets no rotation. Confirm against the engine's
   `Get/SetStdOrientation` traffic in the log rather than by eye.

**Reference device:** the HP 10 G2 Tablet (MT8127, Android 5.0.1) is on hand. Dead Space needs
`armeabi`, which it runs, so the original apk can be installed there for a side-by-side — the
fastest way to answer "is this supposed to look/sound/run like this?" (playbook §4).

---

## 6. Device trail (2026-09-15, runs ds-01…ds-06)

One package install (174 MB), then binary-only pushes. **It boots.**

| run | change | result |
|---|---|---|
| ds-01 | first launch | Boots clean, 24/24 natives, `chdir` to the content root OK, engine constants fetched (`down=393228 up=524300 move=262156`), audio device open, **60 fps** — and a black screen. Tracer named four gaps. |
| ds-02 | answer `GetInstance` / `getAssets`, dispatch `Startup(AssetManager)`, give the delegates real objects | The engine's own `EAIO_Startup` call arrived and was correctly deduped against the module's pre-call. Still black; tracer now named `isContentReady`, `AssetManager.open/openFd`. |
| ds-03 | `isContentReady -> true` (it is just `Query.contentReady`, the flag the download flow sets; our content is staged before the process starts), log asset names | Asset names revealed: **`EAMCore.ini`** — which `ds-stage.sh` had stripped out of the apk — and **`data.zip`**, which this SKU genuinely does not ship. |
| ds-04 | serve `AssetManager.open()` as a real `java.io.InputStream` over staged files | It reads the ini, spawns 5 threads, **writes its first 2048 bytes of PCM** — then **SIGSEGV**. And the stack clamp fired exactly as §4 risk 2 predicted: `setstacksize(32768) too small for a glibc thread -> 131072`. |
| ds-05 | `APKENV_TRACE_FILES=1` | **The engine opens no content file at all** — only `/proc/cpuinfo`. Probe verified to cover both `fopen` and `open`, and failures are logged unconditionally, so this is a real negative, not a blind spot. |
| ds-06 | throw `IOException` on a failed `AssetManager.open` (Android throws; returning NULL is a wrong answer, playbook §4) | **No change** — same crash, same address. Theory spent; do not re-chase it. |

### The open bug

```
signal 11 addr=(nil)  pc = libDeadSpace.so +0x35b570   r3 = 0
  0x35b564: ldr r3, [sl]        ; sl is a stack slot -> NULL
  0x35b570: ldr r1, [r3]        ; <- faults
```

The enclosing function builds textures — its named callees are
`m3g::Texture2D::Texture2D(m3g::Image2D*)`, `setFiltering`, `setWrapping`,
`eastl::vector<intrusive_ptr<m3g::Texture2D>>::reserve/DoInsertValue`, `GetAllocatorForCore`,
`m3g::Object3D::getUserData`. The crash stack has Adreno frames (`leia_sethwstate_enables`,
`rb_state_enables` in `libGLESv2.so`, which `libGLES_CM.so` sits on top of), so it is touching real
GL at the time.

**`[BLAST] first NativeOnDrawFrame returned` is in the log** — frame 1 completes. This dies on
frame 2, building Texture2D objects, having loaded no texture from disk.

**Ruled out so far:** the dual-GL-table hazard (no `[GLBIND]` line and no "shared names" rebinding
here, unlike the ES2 ports — the ES1 table is used directly, which is right for this engine); a
missing/failed content path (nothing is opened, and nothing *fails* to open); a missing
`IOException` on the `data.zip` probe.

**Next, in order:**
1. The engine queries `GL_NUM_COMPRESSED_TEXTURE_FORMATS` / `GL_COMPRESSED_TEXTURE_FORMATS`
   (strings confirmed in the binary). If the ES1 wrapper does not answer that `glGetIntegerv`, an
   uninitialised count or a zero-length array is an easy route to the NULL seen here. Check the
   wrapper, and log what the engine gets back.
2. `APKENV_GL_UPLOADCHECK` for `glGetError` around uploads, and a GL call trace over frames 1–2 —
   frame 1 works, so a diff between the two frames is cheap and likely decisive.
3. `openFd("EAMCore.ini")` still returns NULL+exception even though the file exists; the engine may
   prefer the fd path for mapping. Serving a real `AssetFileDescriptor` is the obvious next
   contract point if 1 and 2 come up empty.
4. Reference device: install the original apk on the HP 10 G2 Tablet and watch `logcat` through the
   same two frames. It runs `armeabi`, so it will take this build.
