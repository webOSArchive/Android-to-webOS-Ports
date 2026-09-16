# Star Wars: Tiny Death Star — Cocos2d-x 2.0.4 port (2026-09-16)

Package `com.apkenv.tinydeathstar` from `android-candidates/tiny-deathstar_1.4.2.apk`, which is
really **1.4.1** (versionCode 16; the filename is wrong). The 1.2.2 apk has the same host contract
and is a fallback only. Module: `apkenv/modules/cocos2dx.c`. Logs: `plan/logs/tds-*.log`.

## Triage
- `lib/armeabi-v7a/libgame.so` (6.2 MB) = **cocos2d-2.0-x-2.0.4** plus the game; ES2, **no EGL
  imports** (the host owns the context). Stock `org.cocos2dx.lib` Java host plus the game's activity
  `com.lucasarts.tinydeathstar.tds`.
- Assets are loose in the apk's `assets/` (PNG RGBA, plist, json). The engine reads the apk itself
  through its own minizip (`nativeSetApkPath`), so it needs no AssetManager.
- Audio: FMOD Ex + FMOD Event (`.fev` plus four `.fsb`). This libfmodex autodetects **OpenSL** if
  `dlopen("libOpenSLES.so")` succeeds and falls back to AudioTrack otherwise. The dex has no
  `org.fmod.FMODAudioDevice`, so the game shipped on OpenSL.
- Online stack: Playdom MSDK (native libcurl), DMO IAP, Tapjoy, Burstly, UrbanAirship. The device is
  offline, and the game already handles that state.

## Host contract (read off the smali before the first run)
| Java side | What the module does |
|---|---|
| `System.loadLibrary` → `JNI_OnLoad` (libDMOIAPManager, libgame) | Calls both. libgame's JniHelper stores the JavaVM there. |
| `Cocos2dxHelper.init` → `nativeSetApkPath(sourceDir)`, `nativeSetExternalAssetPath(<sdcard>/Android/data/<pkg>/files/assets/)` | Passes the apk path, and a dir under the data home for the external path. |
| `onSurfaceCreated` → `nativeInit(w, h)` | Here these really are view pixels: `setScreenWidthAndHeight` comes from `onSizeChanged`. We pass 768×1024. |
| `onDrawFrame` → `nativeRender()` + sleep to `sAnimationInterval` | Calls `nativeRender`, then paces to `setAnimationInterval` (the game asks for 1/30). |
| `onTouchEvent` → `nativeTouchesBegin/End(id,x,y)`, `Move/Cancel([I[F[F)` | Passes view pixels, **not normalized**, rotated with the present. |
| `Cocos2dxBitmap.createTextBitmap(text, font, size, align, w, h)` → `nativeInitBitmapDC(w,h,byte[])` | Re-implemented on FreeType, following the Java layout arithmetic line for line. |
| `getCocos2dxWritablePath` = `Context.getDir("data")` | Returns `<home>app_data` and **creates it**. |
| `tds.isOnline` / `isTablet` / `formatNumber` / `getUUIDv4` / locale / version | false / true / "1,234" / random upper-case UUID / en_US / 1.4.1 |
| `IAPBridge.GetStoreType` (polled every frame) | 0 = `kStoreType_Unknown`, the value Java itself returns when the store is neither googleplay nor amazon. |
| ads, analytics, notifications, wake lock, accelerometer, SimpleAudioEngine | Logged no-ops. The game plays its sound through FMOD. |
| `tds.soundBoardBackground/Foreground` | Called on card pause and resume. |

## Device runs
- **tds-01**: boots, renders portrait, and shows the game's own "Allow Push Notifications?" dialog.
  The OpenSL shim worked at once, but FMOD destroyed its player after 47 buffers.
- **tds-02** (`APKENV_TRACE_FILES`): SoundBoard copies its banks from the apk to
  `getWritablePath()/…` and that fopen failed, because `app_data` did not exist.
  `recursive_mkdir` only creates up to the last `/`. Fixed.
- **tds-03**: the directory now exists, but all five banks were copied as **0 bytes**. The autotap
  on "NO" worked and the tutorial ran.
- **tds-04..06**: a `writev` trace settled it: `writev(fd=0, …) -> 5247584`. The banks were
  being written to **stdin**.
  **Cause:** the gnustl `std::ofstream` compiled into libgame reads `fp->_file` directly (bionic's
  `fileno` is a macro: a `short` at offset 14). A glibc `FILE` has the upper half of
  `_IO_read_base` there, which is 0 on a fresh stream.
- **tds-08**: added **bionic-layout FILE proxies** (`compat/libc_wrappers.c`, opt-in via
  `apkenv_bionic_stdio_enable()`). Banks copy at full size, FMOD stays up, audio drains at exactly
  real time (96 KB/s = 24 kHz stereo S16), 0 underruns. **The user confirmed music playing and touch
  working on the panel.**
- **tds-09**: frame pacing brought a steady 29 fps. The ~7–8 s first `nativeRender` is the tower
  load, and progress persisted across the restart.
- **tds-10**: the LucasArts loading splash (`res/drawable-xhdpi/splash.png`, fitCenter on black, as
  main.xml's loading ImageView does) is up before `nativeInit` and retires on the first draw.
- **tds-11**: the fallback system font is now Prelude (56 KB). ArialUnicode (23 MB in RAM) is kept
  as second choice.

## Lessons worth carrying
1. **bionic's stdio macros read the FILE struct.** Anything built against bionic headers can inline
   `fileno`/`feof`/`ferror` as field reads. apkenv returned glibc `FILE`s, so those reads returned
   garbage that looked plausible (fd 0), and the writes "succeeded" into stdin. The symptom was a
   correctly named, correctly created, empty file. Opt in with `apkenv_bionic_stdio_enable()`.
2. **Trace the write side too.** The open-only file trace said every open was "ok". Only a
   `writev` trace showed where the bytes went.
3. **`recursive_mkdir(path)` needs a trailing slash** to create the last component.
4. **A shim can be the cheaper path even when the engine has a fallback.** FMOD would have fallen
   back to AudioTrack, but the game shipped on OpenSL. `compat/opensles.c` is now a general sink,
   gated per module, with a host unit test (`tools/openslestest.c`).

## Rebuild / package
```
PATH=<cross gcc>:$PATH apkenv/build-webos.sh
ASSET=res/drawable-xhdpi/splash.png NAME=tinydeathstar \
  apkenv/tools/tr2-extract-splash.sh packaging/tinydeathstar.apk packaging/extras/tinydeathstar 768x1024 contain
APPID=com.apkenv.tinydeathstar APK=packaging/tinydeathstar.apk \
  APPINFO=packaging/tinydeathstar/appinfo.json ENVFILE=packaging/tinydeathstar/apkenv.env \
  EXTRAS=packaging/extras/tinydeathstar README=packaging/tinydeathstar/README packaging/build-ipk.sh
```
(Run from `apkenv/`.) Host note: `gcc-13-arm-linux-gnueabi` was apt-removed on 2026-09-15. This
session used a copy extracted with `apt-get download` + `dpkg -x` into the scratchpad, linked to the
still-installed `cc1` and binutils. Reinstall the package to restore the documented build.

Test drivers: `APKENV_COCOS_AUTOTAP="x,y@frame;x1,y1>x2,y2@frame"` (panel pixels, through the real
input path), `APKENV_COCOS_NO_PACING=1`, `APKENV_COCOS_LANDSCAPE=1`, `APKENV_COCOS_DEFAULT_FONT=`.
