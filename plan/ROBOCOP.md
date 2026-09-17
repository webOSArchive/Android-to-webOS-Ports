# RoboCop 3.0.6 (Glu, Unity 4.2.2f1 + Mono) — port trail

Scoping (static read of apk + OBB): `plan/ROBOCOP-SCOPING.md`. Reference behaviour: the game runs
offline under OpenMobile ACL on the TouchPad.

Working copies (gitignored): `apkenv/packaging/robocop.apk` (= the pristine candidate),
`apkenv/packaging/extras/robocop/main.1309.com.glu.robocop.obb` (md5
`00da7034bad96d2ac90fe097583cd77f`, copied unmodified from `android-candidates/`).

## Build + package
```
cd apkenv
tools/build-mono-webos.sh unity4.2      # -> hostlibs/unity42/libmono-unity42.so (once)
./build-webos.sh
APPID=com.apkenv.robocop APK=packaging/robocop.apk APPINFO=packaging/robocop/appinfo.json \
  ENVFILE=packaging/robocop/apkenv.env HOSTLIBS=hostlibs/unity42 EXTRAS=packaging/extras/robocop \
  packaging/build-ipk.sh
APPID=com.apkenv.robocop WAIT=60 tools/tr2-run.sh robocop-rN     # full install + launch + log
APPID=com.apkenv.robocop tools/push-run.sh robocop-rN            # binary-only cycle
```

## Stage M — the Unity 4.2 Mono runtime (2026-09-17)

| Fact | Value | Evidence |
|---|---|---|
| Source | `Unity-Technologies/mono` branch **`unity-4.2`** @ `ac09c9bb` (2013-08-12): `AM_INIT_AUTOMAKE(mono,2.6.5)`, `MONO_CORLIB_VERSION 82` | `configure.in`, `appdomain.c` |
| Where the new exports live | `unity/unity_liveness.c`, `unity/unity_utils.c`, already listed in `mono/metadata/Makefile.am` | grep |
| Build | `tools/build-mono-webos.sh unity4.2` — same flags and patch as unity3.5, applies cleanly | build log |
| **Export set** | host build **931 = the game's bionic libmono 931, zero difference either way** | `nm -D` comm |
| libunity → libmono | 129 names (`plan/robocop-mono-imports.txt`), all exported | `check-mono-exports.sh` |

The shipped ports keep `hostlibs/webos/libmono-webos.so` (unity3.5). RoboCop ships
`hostlibs/unity42/libmono-unity42.so`, packaged as `hostlibs/webos/libmono-unity42.so`.

**Trap avoided:** `apkenv_hostlib_bridge()` aborts on any missing symbol, and
`compat/mono_symbols.h` is the union of every game's import list. Adding RoboCop's list as-is would
have made the next TR2/Aralon binary abort at startup (the unity3.5 runtime lacks
`mono_unity_liveness_*`). `tools/gen-mono-hooklist.sh` now splits the union: **required** = names
the shipped unity3.5 runtime exports (still fatal if missing), **optional** = the rest, bridged by
`apkenv_hostlib_bridge_optional()` only when the loaded runtime has them.

## Log
- **r1 (`logs/robocop-r1.log`)**: Mono bridge 131/131 + 6/6 optional. Then SIGSEGV at pc=0: apkenv
  loaded the apk's 47 KB **proxy** libunity (its `JNI_OnLoad` looks for
  `assets/libs/armeabi-v7a/libunity.so`, finds nothing, registers no natives). The real engine is only
  in the OBB. Fix: `tools/rc-stage.sh` adds the OBB's `libunity.so`/`libmono.so` and the apk's
  `libmain.so` (a NEEDED of libunity) at `assets/libs/armeabi-v7a/` in the working apk.
- **r2**: engine up — `unity4 host: yes`, `unityAndroidInit -> 1`, shader renderer chosen by itself,
  **the MGM logo renders full-screen landscape** (`grab.sh`). Boot then threw in `Boot.Awake →
  StorageManager.PersistentDataPath → AJavaTools.GetFilesPath → JniUtils.FindClass`:
  `FromReflectedMethod (foreign object) -> NULL`. **Cause: Unity 4.2 makes every AndroidJavaObject
  call through the jvalue[] ("A") JNI forms**, including `ReflectionHelper.getMethodID`; Aralon's
  4.0 used the va_list forms, which were the only ones answered. Fix: `un_unity42` (gated on the
  4.2-only native `nativeSetDefaultDisplay`, so Aralon is untouched), an argument cursor shared by
  both forms, A-form overrides for every return type dispatching into `un_java_call()` (class +
  method name), `Class.forName` → a `java/lang/Class` object describing the class, and a bounded
  `[UN-JAVA] unanswered <kind> <class>.<method><sig>` line for every call nobody answered.
- **r3–r6**: answered one named gap per run from the static contract: `GetFilesPath`/
  `GetExternalFilesPath` (→ `<data>/files`, plus `<data>/shared_prefs` which GWalletHelper writes
  via `files/../shared_prefs`), `GetDeviceLanguage/Country`, `GetVersionName/Code` (env
  `APKENV_UNITY_VERSION_NAME/_CODE`), `GetRunCount` (host counter in PlayerPrefs),
  `IsPlayGameServicesAvailable` → 1, `GetOBBDownloadPlan` → `"old"`, `GetAndroidID`,
  `SystemClock.elapsedRealtime`, `Locale.getDefault().getISO3Language()` (engine-native; NULL
  aborted libunity in `std::string`). Unanswered object-returning plugin calls (e.g.
  `UnityAdsFactory.createAdvertising`) now return a **stand-in object of the declared class** —
  NULL threw "Init'd AndroidJavaObject with null ptr!" and aborted `App.Awake`. Constructors on
  `Class.forName` classes were named from heap garbage (`un_construct` read a Class object as a
  dummy class) — fixed. A void call through a NULL jmethodID (from libunity) faulted at
  `method->name`; the V handlers now log and ignore it.
  **State after r6:** every plugin init passes, no crash; the screen shows Glu's pulsing
  **"SLOW INTERNET CONNECTION"** (user saw it too): `Boot:<Init>m__45(Exception)` ←
  `DynamicContentPipeline.DoContentUpdate` fails, then `DLCHandler.FillPacksList` NREs. Under ACL
  offline the game boots — so the host answers something differently on that path (being traced).
- **r7–r10 — the content pipeline, found by static trace + strace.** A second contract pass traced
  `DynamicContentPipeline.DoContentUpdate`: offline, the A/B test resolves from
  `WWW("jar:file://<dataPath>!/assets/ABTesting.xml")` and the bundle index from
  `…!/assets/AssetBundles/`, which exist only in the OBB. Glu's `UnityLauncherActivity` overrides
  `getPackageCodePath()` to return the OBB, so the device's ONLY `nativeFile` is the OBB →
  `APKENV_UNITY_OBB_CODEPATH=1`. Not sufficient alone (r7). `APKENV_MONO_TRACE=<spec>` (Mono's
  `--trace`, via the bridged `mono_jit_set_trace_options`) showed 34 `IsolatedStorageException`s but
  missed the pipeline (C# iterators are nested types, namespace ""). **apkenv's file tracer is blind to
  managed I/O** — the host Mono calls glibc directly — so `strace -f -p` on the device (it has
  `/usr/bin/strace`) was the probe: the pipeline looked for `cache/<buildTag>/ABTestingResolutionData.dat`
  and never created the directory, i.e. resolution failed.
- **r11 — the cause: apkenv had no WWW at all.** On Android `UnityEngine.WWW` is Java: the host calls
  `nativeInitWWW(WWW.class)`, each request is `new com.unity3d.player.WWW(id, url, post, headers)`,
  a Thread streaming back via the static natives. We never called `nativeInitWWW`, so every WWW —
  local ones included — never completed. Host WWW in `unity.c` (4.2-gated), from the game's
  `WWW.run()` smali: `file://` and `jar:file://<zip>!/<entry>` are served (content-length header,
  a zero-length `readCallback` first, 32 KB chunks, progress, done); network URLs fail with
  Android's offline `UnknownHostException` text. `Thread.join()` really joins. **→ A/B + index
  bundles load from the OBB, the RoboCop logo screen, then the game.** DLC bundles are online-only
  (only their `.version` files are in the OBB) and fail as on an offline device.
- **r12–r13 — P/Invoke into apk libraries.** `[DllImport("gwallet")]` hit Mono's glibc `dlopen`,
  which cannot load a bionic .so → `TypeInitializationException` in `Tutorial.Init`. `apkenv.c` now
  registers a **Mono dl fallback** (only if the runtime exports `mono_dl_fallback_register`; the
  shipped 3.5 runtime does not) that loads through apkenv's linker and, like
  `System.loadLibrary`, **runs the library's `JNI_OnLoad`** (without it libgwallet faulted on a NULL
  JavaVM in `GWalletCallbackJNI::initialise`). `rc-stage.sh` stages `libgwallet.so`.
  **r13: "WELCOME ROBOCOP" tutorial over the 3D scene. User: taps work, tutorial advances, music
  plays.**
- **r14:** `FpsCounter.Update → NativeUtils.GetCurrentMemoryBytes` threw on nearly every frame
  (`getProcessMemoryInfo` → NULL array): answered (`MemoryInfo[1]`, `getTotalPss` = VmRSS), plus
  `GetObjectArrayElement` for our arrays. Zero exceptions; **29.7 fps steady** (Unity 4's default
  Android cap of 30). The render log line now carries fps.
- **User-confirmed on the panel (2026-09-17, r14 binary on package 0.1.0):** touch, tutorial,
  music, **aiming, SFX, a full mission completed**. Not yet run: the pause/resume and swipe-away
  lifecycle protocol, and a fresh install from a rebuilt package (0.1.0 was installed before the
  r14 binary was pushed over it). Icons: `apkenv/packaging/out/icons/robocop/`.
- **RELEASE 1.0.0 (2026-09-17):** user ran the pause/resume and swipe-away protocol.
  `apkenv/packaging/out/com.apkenv.robocop_1.0.0_all.ipk` (94.6 MB: staged apk 17.7 MB + unmodified
  OBB md5 `00da7034…` + `libmono-unity42.so`). Its `apkenv` (md5 `2cd1839c…`) is byte-identical to the
  r14 binary the user played on. Not yet fresh-installed from the package.
