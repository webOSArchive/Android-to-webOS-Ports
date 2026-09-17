# Third-party components

This repository contains or builds on the following third-party work. Each keeps its own license;
see the files named below.

| Component | Where | License |
|---|---|---|
| **apkenv** by Thomas Perl — the base of this toolkit | `apkenv/` (see `apkenv/LICENSE.apkenv`) | BSD 2-clause |
| wrapper-generator (part of apkenv) | `apkenv/wrapper-generator/` (`COPYING`) | BSD 2-clause |
| Android bionic dynamic linker | `apkenv/linker/` (`NOTICE`) | BSD-style, © The Android Open Source Project |
| Android bionic runtime libraries (libc, libm, libstdc++, liblog, libz) | `apkenv/libs/webos/` (prebuilt) | BSD-style / Apache 2.0 (AOSP), zlib license (libz) |
| Other AOSP headers and sources (resource types, keycodes, Dalvik compat) | `apkenv/apklib/resource_types.h`, `apkenv/compat/android_keycodes.h`, `apkenv/android/` | Apache 2.0, © The Android Open Source Project |
| minizip (Gilles Vollant, Even Rouault) | `apkenv/apklib/unzip.*`, `ioapi*` | zlib license |
| stb_image (Sean Barrett) | `apkenv/imagelib/stb_image.h` | public domain / MIT |
| Khronos EGL / OpenGL ES headers | `apkenv/glshim/{EGL,GLES,GLES2,KHR}/` | Apache 2.0 / Khronos license, © The Khronos Group |
| **Mono runtime** (Unity-Technologies fork) | `apkenv/hostlibs/webos/libmono-webos.so`, `apkenv/hostlibs/unity42/libmono-unity42.so` (prebuilt; see `apkenv/hostlibs/README`) | GNU LGPL v2. Source: https://github.com/Unity-Technologies/mono at the commits in `apkenv/tools/build-mono-webos.sh`, plus `apkenv/tools/mono-webos.patch` |

**Not included, by design:** game code or content of any kind (apks, OBBs, extracted engine
libraries, art, audio), and HP's proprietary `libEGL.so`, which `apkenv/build-webos.sh` harvests from
your own device at build time. See `.gitignore` for the policy.
