# Dead Space (EA Mobile, BLAST engine) — port plan

Target: **`android-candidates/Dead-Space.apk`** — `com.eamobile.deadspace_full_azn` 1.2.0
(versionCode 1200), the **Amazon Appstore** build, dated 2013-10-22. 296 MB.

Status: **triage + host contract done, no device work yet.** Started 2026-09-15 evening following
`PORTING-PLAYBOOK.md` §1–§2. Everything below is static analysis; nothing here has been run.

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

**Open, and the first thing to settle tomorrow:** which delegate feeds the `published/` prefix.
Disassemble around the `published/sounds/soundBase.sb` literal's xref and see what it is
concatenated with.

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

## 5. Plan for the next session

1. Settle §3: disassemble the `published/sounds/soundBase.sb` xref, learn the path root, and decide
   ship-in-place vs `DATA=` seeding.
2. `modules/eablast.c` — name it for the **engine**, not the game (`com.ea.blast` is shared across
   EA's Android ports of this era, so the module should carry over). Call `JNI_OnLoad` first
   (playbook §2), then `EAThread_Init` → `rwfilesystem_Startup` → `EAIO_Startup` → `NativeOnCreate`
   → surface created/changed → per-frame `NativeOnDrawFrame`. Ship the unhandled-call tracer and a
   log line per contract point before the first run.
3. Answer the constant getters (`NativeGetIdRawPointer*`) and feed `NativeOnPointerEvent` the values
   the engine gave, not Android's.
4. `APKENV_PTHREAD_STACK_CLAMP=1` on from run one.
5. Package with `DATA=`/`EXTRAS=` as §3 decides; `requiredMemory` set from a measured RSS.
6. Copy the synthetic-input hook (`APKENV_MORTAR_AUTOTAP`, `modules/mortar.c`) into the new module —
   it is engine-independent and it is what made Fruit Ninja's menus testable without a person.

**Reference device:** the HP 10 G2 Tablet (MT8127, Android 5.0.1) is on hand. Dead Space needs
`armeabi`, which it runs, so the original apk can be installed there for a side-by-side — the
fastest way to answer "is this supposed to look/sound/run like this?" (playbook §4).
