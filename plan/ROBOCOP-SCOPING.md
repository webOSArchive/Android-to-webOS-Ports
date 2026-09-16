# RoboCop 3.0.6 (Glu) — scoping only, 2026-09-16

Not started. This page records the static read of the apk and OBB, so the next session starts from
facts.

## Inputs
- `android-candidates/robocop_3.0.6.apk`: 3.8 MB, `com.glu.robocop`, versionCode 1309, minSdk 9,
  target 16, ES 2.0, landscape (screenOrientation 0/6).
- `android-candidates/main.1309.com.glu.robocop.obb`: an 85 MB zip, 299 MB uncompressed, 1711
  files, built 2015-01-29.
- **Runs offline under OpenMobile ACL on the TouchPad** (user-confirmed). Use that as the reference
  for behaviour, screens and speed on this exact hardware.

## Engine
- **Unity 4.2.2f1** (`mainData` header). The apk's `lib/armeabi-v7a/libunity.so` (47 KB) and
  `libmono.so` (93 KB) are **proxies**. The real engine is `assets/libs/armeabi-v7a/libunity.so`
  (9.5 MB) and `libmono.so` (3.9 MB), **inside the OBB** (the Temple Run 2 lesson: check file sizes
  before believing a grep).
- libunity NEEDED: libmono, libmain, libc, libm, libdl, liblog, libGLESv1_CM, libGLESv2,
  libstdc++, libz. It has no EGL/AInput/ALooper imports.
- Exports `JNI_OnLoad` + FMOD's `Java_org_fmod_FMODAudioDevice_*` (the AudioTrack pump
  path, `audio/fmod_pump.c`, as WMW/TR2). Its native table strings include `nativeTouch`,
  `nativeRender`, `nativeResize`, `nativeInit`, `nativeSetInputCanceled` (the Unity 4 host marker,
  see Aralon), `nativeFile`, `nativeRecreateGfxState`, `nativeFocusChanged`, `nativePause`, `nativeDone`.
- It also exports `ANativeActivity_onCreate`. The manifest uses `UnityPlayerNativeActivity`, but
  the Java UnityPlayer natives are registered too, so `modules/unity.c`'s model should hold.
- Launch chain on Android: Google's expansion DownloaderActivity (LVL) → Glu `UnityLauncherActivity`
  → the Unity player. Skip all of it and drive the player directly.

## Gaps, ranked
1. **Host Mono is too old.** libunity imports 129 symbols from Mono; 6 are missing from
   `hostlibs/webos/libmono-webos.so`: `mono_unity_class_is_abstract`, `mono_unity_class_is_interface`,
   `mono_unity_liveness_calculation_begin/_end/_from_root/_from_statics`. The game's libmono exports
   933 symbols against our 912. Rebuild from Unity's 4.2-era mono branch (`tools/build-mono-webos.sh`)
   and verify the **export sets match** (the TR2 method), then add `plan/robocop-mono-imports.txt`
   and regenerate `compat/mono_symbols.h`.
2. **Glu plugin layer through AndroidJavaObject.** About 50 Java class names appear in
   `Assembly-CSharp*.dll`: `com.glu.plugins.AJavaTools`, `AJTDeviceInfo`, `AJTInternet`,
   `AJTGameInfo`, `AJTUtil`, `AJTUI`, `AJTBackup`, `aads.*` (ads/gifting/video), `asocial.*`
   (Facebook, Game Circle, Play Games), `ainapppurchase`, `anotificationmanager`. IL-scan with
   `tools/ildump.py` for the exact method calls, and extend `unity.c`'s reflection bridge. Use ACL's
   behaviour to decide which answers matter.
3. **Texture compression**: not checked yet. ETC1/ATC are native on the Adreno 220; DXT/PVRTC would
   need a CPU decode.
4. **Online strings** ("Unable to connect to the server… internet connection is required") are
   feature-level. ACL shows the game plays offline.

## Packaging precedent
Aralon: ship the OBB unmodified via `EXTRAS=`, and set `APKENV_UNITY_OBB` + `APKENV_UNITY_PACKAGE`.
Budget ~5 min of USB copy and ~3 min of unpacking per install. (Dead Space had **no** OBB: its content
was inside the apk, which is a different case.)
