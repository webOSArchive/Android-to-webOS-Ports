# Android-to-webOS Ports

Run **Android NDK games natively on webOS** (HP TouchPad) — **without ACL**
(OpenMobile's heavyweight Android runtime) and **without Dalvik**. This is a slim
[apkenv](https://github.com/thp/apkenv)-based shim: a gingerbread **bionic
linker loaded as a library** + a **fake-JNI** layer + a **webOS SDL/PDL backend**
+ a small **per-game module**. The game's own native engine `.so` is loaded and
driven directly through its JNI entry points; there is no Java VM.

> **Worked example — Where's My Water?** boots, renders full-screen upright
> **portrait**, is **touch-playable** (carve-by-drag + HUD), has **sound** (FMOD
> → AudioTrack pump), and installs as a **one-tap webOS `.ipk`** that launches
> from the launcher. End-to-end on real hardware.

**This is a toolkit and methodology, not a complete solution.** It is **not an
emulator** and not a one-click converter. It gives you the scaffolding — the
apkenv shim, the webOS backend, the build/packaging pipeline — and one worked
example to learn from. Porting a game is **hands-on work**: every new title needs
its own per-game module and, usually, engine-specific reverse-engineering and
fixes.

**Every Android app is different, and not all can be ported this way.** The
approach fits **native NDK games** whose logic is in C/C++ and that call a small,
stubbable set of JNI entry points. Apps that are mostly **Java/Dalvik**, lean on
Android frameworks (WebView, Play Services, complex UI, heavy audio/media,
DRM/networking), depend on native features webOS lacks, or ship only `arm64`/x86
are poor fits — some are infeasible. **Triage before you invest** (see
`CLAUDE.md`); expect that some games simply won't work.

It is also a **bring-your-own-apk toolkit**: it contains the porter, scripts,
methodology, and docs — **no game code or copyrighted content**. You supply the
`.apk` you own.

---

## How it works

Android NDK games are mostly C/C++ in `lib/armeabi-v7a/*.so`, with a thin Java
`Activity` that calls native JNI entry points (`rendererInit`, `drawFrame`,
`touch*`, lifecycle) and provides system services (input, audio, sensors). webOS
is also ARM Linux with an OpenGL ES stack — so instead of emulating Android, we:

1. **Load the game's native libs** with a gingerbread **bionic linker compiled as
   a library** (so glibc/webOS and bionic/Android coexist in one process).
2. **Fake the JNI** the engine calls — `JNINativeInterface` implemented in C
   (`FindClass`/`RegisterNatives`/arrays/`NewDirectByteBuffer`/…), so the engine
   thinks a Java VM answered.
3. **Drive the engine from a per-game module** (`apkenv/modules/<game>.c`): call
   its real entry points with hand-faked arguments, since there is no Activity.
4. **Back the Android contracts with webOS primitives** — **PDL + SDL 1.2 +
   GLES_CM** for display/input, an **AudioTrack-style PCM sink + FMOD device
   pump** for audio, render-to-portrait-FBO for orientation, etc.
5. **Package** the porter + the game as a webOS PDK `.ipk`.

The deeper strategy (treating apkenv as a faithful Gingerbread *contract host*,
not a per-game puppet) is in [`android-runtime-architecture.md`](android-runtime-architecture.md);
the hands-on field guide is [`android-port-shim.md`](android-port-shim.md).

---

## Prerequisites

- **A HP TouchPad** in **Developer Mode**, connected over **USB (novacom)**.
  Required for the **first build** (to harvest one proprietary lib — see below)
  and to install/run.
- **PalmPDK** at `/opt/PalmPDK` — headers + ARM device libs for linking, and the
  `arm-gcc` 4.3.3 cross-compiler used at the link step.
- **PalmSDK** — `palm-package`, `palm-install`, `novacom` (the webOS SDK tools).
- **Host cross-compiler** `arm-linux-gnueabi-gcc-13` for the compile step. (The
  build uses a deliberate **two-toolchain** trick — compile with gcc-13 forced
  onto PalmPDK's old glibc-2.4 headers, link with PalmPDK gcc 4.3.3 — so symbols
  bind to the device's glibc 2.4. The rationale is documented at the top of
  `apkenv/build-webos.sh`; don't "simplify" it.)
- Standard host tools: `unzip`, and optionally `baksmali` (to read a game's
  `classes.dex` when writing a new module) and `imagemagick` (`convert`, to
  resize a launcher icon).

---

## Build

```sh
cd apkenv
./build-webos.sh
```

**A connected device is required for the first build.** HP's `libEGL.so` is
proprietary and not in the PalmPDK SDK, so `build-webos.sh` **harvests it from
the device over novacom** into `apkenv/devlibs/` and caches it. If no device is
connected (and `libEGL.so` isn't already cached), the build **fails fast** with a
clear message. Subsequent builds reuse the cache and need no device.

Output: `apkenv/apkenv` — an ARM binary, interp `/lib/ld-linux.so.3`, highest
symbol `GLIBC_2.4` (device-clean).

The committed **FOSS bionic runtime libs** (`apkenv/libs/webos/`: libc, libm,
libstdc++, liblog, libz — the Android system libs the engine was built against)
are loaded by apkenv's own linker at runtime; they are not linked into the host
binary.

---

## Package & install

```sh
# put the game apk you want to ship at apkenv/packaging/<game>.apk, then:
cd apkenv
APK=packaging/your-game.apk ./packaging/build-ipk.sh      # -> packaging/out/*.ipk
palm-install packaging/out/<app>_<ver>_all.ipk            # installs over novacom
```

Then **tap the icon** in the launcher. The packaged app is self-contained: the
app dir holds the binary, the FOSS host libs (`libs/webos/`), and the game
(`android/<game>.apk`); writable data (saves, the apk's runtime-extracted libs,
logs) goes to `/media/internal/.apkenv/`.

Reinstalling? **Bump `version` in `appinfo.json`** — `palm-install` silently
refuses a same-or-lower version.

> **How webOS actually launches a PDK app** (jail, lost stdio, launch-params as
> argv, …) bit us repeatedly and is written up in `apkenv/BUILD-STATE.md` and
> upstreamed to the webOS-MCP knowledge base. The binary already self-locates via
> `/proc/self/exe`, redirects its log to `/media/internal/`, and forces the
> bundled apk — so it "just works" from the launcher.

---

## Porting a new game

1. **Triage the apk** (see `CLAUDE.md`): native NDK engine, GLES, few JNI classes
   to stub. Drop it in `android-candidates/`.
2. **Find the entry points** — `baksmali classes.dex` for the JNI signatures the
   Activity calls; `readelf`/`objdump` on the engine `.so` for its exports.
3. **Write `apkenv/modules/<game>.c`** — a per-game module that calls those entry
   points (init → resize → loop(drawFrame) → feed touch/lifecycle). Carry
   **facts, not behavior** (entry points, portrait flag, asset root); push any
   missing *behavior* into the apkenv subsystem (input/audio/orientation), not the
   module. The staged methodology + review checklists are in `plan/`.
4. Build, package, install, iterate on-device (logs land in
   `/media/internal/apkenv-*.log`).

---

## Distribution & licensing

- **The porter is FOSS.** apkenv is BSD-licensed (`apkenv/LICENSE.apkenv`); the
  committed bionic runtime libs are AOSP/zlib (FOSS).
- **No game content is in this repo.** `.apk`, `.ipk` (it bundles the game),
  patched/extracted game binaries, and game art are git-ignored.
- **`libEGL.so` is HP-proprietary** — harvested from your own device at build
  time, never committed/redistributed.
- **Bundling vs. bring-your-own.** Bundling the apk inside the `.ipk` is fine for
  **free / archival** games. For **paid/proprietary** titles the apk must stay
  **external** (user supplies their own — the emulator-and-your-own-ROMs model);
  that distribution path is designed in `plan/STAGE-6-packaging.md` §8.

---

## Status

| Area | State |
|---|---|
| Boot / load native engine | ✅ |
| Display — full-screen upright portrait | ✅ (render-to-portrait-FBO) |
| Input — menus + in-level touch (carve + HUD) | ✅ |
| Audio — music + SFX | ✅ (FMOD AudioTrack pump → lock-free ring → SDL) |
| Packaging — one-tap `.ipk` from the launcher | ✅ |
| Generalize to a 2nd game / de-hack | ☐ next (`plan/STAGE-5`) |

Worked example **Where's My Water?** is playable end-to-end with sound. See
`apkenv/BUILD-STATE.md` and `plan/` for the full state and what's next.

---

*Built on [thp/apkenv](https://github.com/thp/apkenv). Sibling project: the Palm
Pre→TouchPad `.ipk`-patching track for native webOS games.*
