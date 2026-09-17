---
name: acl-anatomy
description: "What OpenMobile ACL actually is internally, and which parts to harvest for a slim Android-NDK→webOS game wrapper"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 7a813961-5d5f-46b3-b86d-ff6f66990032
---

OpenMobile ACL (Application Compatibility Layer) for HP TouchPad, dissected from `github.com/webOSArchive/acl`. That repo = the **offline-activation patch + packaging**, not source. Source is lost but the user (webOS Archive owner) has **decompile permission** from the defunct original team — RE of the binaries is OK.

**Install layout:** self-extracting `acl-1.2.0.3-webos-installer.bin` → `android-fs.tar.gz` → creates webOS-side `/media/omww/` + a **150MB ext3 `system.img`** (`/media/cryptofs/omww/system.img`) = a full Android root. Extract image without root via `debugfs -R "..." system.img`.

**ACL is NOT a thin shim — it's a complete Android 2.3.6 (Gingerbread, SDK 10, armeabi-v7a) userspace** bridged to webOS:
- Full bionic + Dalvik: `/bin/linker`, bionic libc/libdl/libm/liblog, libdvm.so, app_process, dalvikvm, dexopt.
- Full SurfaceFlinger stack: libsurfaceflinger/libgui/libui/libbinder.
- Real GPU drivers Android-side: `lib/egl/libEGL_adreno200.so` + `libGLESv2_adreno200.so` (TouchPad Adreno 220).
- **Bridge HALs** `*.omww.so` (gralloc.omww, sensors.omww, lights.omww, gps.omww) + `libacl_jni.so` talk over sockets to **webOS-native daemons** `omww-{netd,sensord,powerd,alarmd,audio-notification,mapd,proxy,service-mngr,lad}` and the **vfb-agent/vfb-client** pair (webOS-side SDL+PDL+libGLES_CM presenter; VFB socket at `/media/omww/android/opt/omww/dev/vfb`). Path: app→SurfaceFlinger→gralloc.omww→shared buf→vfb-client→screen. That whole Android boot is why ACL is heavy.

**Value as a PARTS DONOR for a slim wrapper** (see [[android-apk-port-triage]], [[templerun2-port-analysis]]): the two scariest unknowns are now PROVEN on-device — **bionic runs on TouchPad** and **Adreno GLES works**. Harvest: ACL's `/bin/linker`+bionic libs (load libunity/libmono — no apkenv loader needed), the `*.omww` HALs + `omww-*` daemons as reference service bridges, vfb-client + gralloc.omww as the display-bridge reference. Discard Dalvik/SurfaceFlinger/binder.

**Graphics shortcut for Unity 3.5:** libunity imports ZERO egl symbols (host creates the GL context) and only issues gl* calls → our native host creates the context with **webOS PDL/EGL** and resolves libunity's ~200 gl* imports to **webOS libGLESv2** (value-in/value-out, trivial cross-libc marshal). Sidesteps gralloc + SurfaceFlinger entirely; zero-copy direct render. Reverse `gralloc.omww.so` only if direct-EGL fails.

**Spike to settle bionic-linker vs Mono-JIT:** dlopen libunity+libmono via ACL linker in a PDK proc, stand up webOS EGL, route gl*→webOS GLESv2, drive `Java_com_unity3d_player_UnityPlayer_*` to clear screen to a color.
