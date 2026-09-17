# Android-to-webOS Ports

Run **Android NDK games natively on webOS** (HP TouchPad) — **without ACL**
(OpenMobile's heavyweight Android runtime) and **without Dalvik**. This is a slim
[apkenv](https://github.com/thp/apkenv)-based shim: a gingerbread **bionic
linker loaded as a library** + a **fake-JNI** layer + a **webOS SDL/PDL backend**
+ a small **per-game module**. The game's own native engine `.so` is loaded and
driven directly through its JNI entry points; there is no Java VM.

> **Nine games run this way today** — from Where's My Water? (the first port) to
> Unity 4.2 shooters like RoboCop — each installed as a **one-tap webOS `.ipk`**
> that launches from the launcher, with touch and sound, on real hardware. See
> [Status](#status).

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
`PORTING-PLAYBOOK.md` and `android-candidates/README.md`); expect that some games simply won't work.

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
  Install it with `sudo apt install gcc-13-arm-linux-gnueabi g++-13-arm-linux-gnueabi`
  (Ubuntu 24.04 was the last build host).
- **Host tools:** `python3` (the build generates `compat/gen/gles_serialize.c`),
  `unzip`/`zip`, `aapt` (packaging reads the launcher icon from the manifest),
  ImageMagick (`convert`, to resize the icon), `xz`.
- **For reverse-engineering a new game:** `baksmali`/`apktool` (read the Java
  host), `readelf`/`objdump`, and `ffmpeg` for the audio/splash extract scripts.
- **Only to rebuild the prebuilt Mono runtimes** in `apkenv/hostlibs/`: autoconf,
  automake, libtool, bison, perl, git and network access
  (`apkenv/tools/build-mono-webos.sh [unity3.5|unity4.2]`).
- PalmPDK and PalmSDK are no longer distributed by HP; you need archived
  installers of both.

**Reading order:** this README → [`PORTING-PLAYBOOK.md`](PORTING-PLAYBOOK.md)
(the method) → [`apkenv/modules/README.md`](apkenv/modules/README.md) (writing
and wiring a module) → the trail for the closest engine in `plan/` →
[`apkenv/ENV-VARS.md`](apkenv/ENV-VARS.md) (every `APKENV_*` switch) →
[`android-port-shim.md`](android-port-shim.md) (field guide).
`android-runtime-architecture.md` and `plan/STAGE-*.md` are background.

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

1. **Triage the apk** (see `PORTING-PLAYBOOK.md` and `android-candidates/README.md`): native NDK engine, GLES, few JNI classes
   to stub. Drop it in `android-candidates/`.
2. **Find the entry points** — `baksmali classes.dex` for the JNI signatures the
   Activity calls; `readelf`/`objdump` on the engine `.so` for its exports.
3. **Write `apkenv/modules/<game>.c`** — a per-game module that calls those entry
   points (init → resize → loop(drawFrame) → feed touch/lifecycle). Carry
   **facts, not behavior** (entry points, portrait flag, asset root); push any
   missing *behavior* into the apkenv subsystem (input/audio/orientation), not the
   module. **Add it to `SOURCES` in `apkenv/build-webos.sh`** — see
   [`apkenv/modules/README.md`](apkenv/modules/README.md) for the skeleton and the
   engine → module table.
4. Build, package, install, iterate on-device: `apkenv/tools/tr2-run.sh` (full
   install + launch + log; generic despite the name), `push-run.sh` (binary-only),
   `grab.sh` (screenshot). Logs land in `/media/internal/apkenv-<appid>.log` and
   are pulled to `plan/logs/`.

**What is maintained:** `apkenv/build-webos.sh`, `platform/webos*.c`, `compat/`,
`jni/`, `linker/`, `audio/`, the modules listed in `build-webos.sh`, `packaging/`
and `tools/`. The other platforms (Pandora, Harmattan, Fremantle, Sailfish,
PocketCHIP, Raspberry Pi, OSMesa), their `makefile`/`debian`/`rpm` build, the
`wrapper-generator/`, and the remaining modules are upstream
[thp/apkenv](https://github.com/thp/apkenv) heritage, kept for reference and not
built for webOS.

---

## Distribution & licensing

- **The porter is FOSS.** apkenv is BSD-licensed (`apkenv/LICENSE.apkenv`); the
  committed bionic runtime libs are AOSP/zlib (FOSS); the prebuilt Mono runtimes
  are LGPL, built from Unity's public Mono fork (`apkenv/hostlibs/README`).
  Third-party attributions: [`NOTICE.md`](NOTICE.md).
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

Nine games ship as one-tap `.ipk`s, each launching from the webOS launcher icon
with touch and sound. The **module** column is where to start when your game uses
the same engine.

| Game | Engine | Module | State |
|---|---|---|---|
| **Where's My Water?** | Disney/native | `wheresmywater.c` | ✅ playable end-to-end, portrait, audio |
| **Plants vs. Zombies HD** | Marmalade/Airplay | `marmalade.c` | ✅ playable, audio, centred letterbox |
| **Amazing Alex HD** | Rovio ka3d | `angrybirds.c` | ✅ playable, audio — ported in one pass |
| **Temple Run 2** | Unity 3.5 + Mono | `unity.c` | ✅ portrait, touch/swipe/tilt, 3D, SFX, music, splash |
| **Aralon: Sword and Shadow HD** | Unity 4.0 + Mono | `unity.c` | ✅ menus, touch, open world, SFX, dialogue, music |
| **Fruit Ninja** | Halfbrick Mortar | `mortar.c` | ✅ every mode, 59 fps, audio, saves persist |
| **Dead Space** | EA BLAST | `eablast.c` | ✅ menus, cutscenes, gameplay, audio (3:2 letterbox) |
| **Star Wars: Tiny Death Star** | Cocos2d-x 2.0 | `cocos2dx.c` | ✅ portrait, touch, text, FMOD music + SFX |
| **RoboCop** | Unity 4.2 + Mono | `unity.c` | ✅ tutorial, aiming, missions, SFX, music, 30 fps |
| Where's My Water? 2 | same as WMW | `wheresmywater2.c` | ☐ reaches the level, stalls on multi-threaded GL loading |

Per-game trails — what broke, how it was found, and the fix — are in `plan/`
(e.g. `plan/ROBOCOP.md`, `plan/FRUITNINJA.md`, `plan/DEAD-SPACE.md`). Each
packaged game's launch settings are a small `apkenv/packaging/<game>/apkenv.env`.

Capabilities the framework now has, general rather than per-game (new
behaviour is gated per engine or opt-in, so shipped ports keep their exact path):

| Area | State |
|---|---|
| Boot / load native engine | ✅ bionic-linker-as-library + fake-JNI, engine `JNI_OnLoad` honoured |
| Display | ✅ native landscape, letterbox, or upright portrait via render-to-FBO (ES1 *and* ES2) |
| Input — touch, swipe, tilt | ✅ PDL touch + PDL sensors |
| Audio | ✅ FMOD AudioTrack pump, OpenSL ES sink (`compat/opensles.c`), lock-free ring → SDL |
| Engine's own language runtime | ✅ host-built Mono bridged in (`compat/hostlib.c`; Unity 3.5 and 4.2 runtimes) |
| Unity host contract | ✅ Unity 3.5 / 4.0 / 4.2 boot orders, AndroidJavaObject reflection (va_list *and* jvalue[] JNI), PlayerPrefs |
| Unity `WWW` | ✅ host implementation: `file://` and `jar:file://` (OBB/apk) served; network fails like an offline device |
| C# `[DllImport]` of apk libraries | ✅ Mono P/Invoke fallback through apkenv's linker, running the library's `JNI_OnLoad` |
| Expansion files (OBB) | ✅ shipped unmodified, fed to the engine as Android would |
| bionic `FILE` ABI | ✅ opt-in bionic-layout stdio proxies (`fileno`/`feof` macros read struct fields) |
| Boot splash during load | ✅ host-drawn, rides the present quad |
| Packaging — one-tap `.ipk` from the launcher | ✅ `packaging/build-ipk.sh` |

Tools for the device loop (`apkenv/tools/`): `push-run.sh` (binary-only cycle,
md5-checked), `grab.sh` (screenshot of the GL frame — the webOS screenshot misses
the GL layer), synthetic input for driving a port with nobody at the device, and
tracers (`APKENV_TRACE_FILES`, `APKENV_MONO_TRACE`, `APKENV_UNITY_ICALL_TRACE`, …).

Start with `PORTING-PLAYBOOK.md` for the method. `plan/` carries the full state
and what's next.

---

*Built on [thp/apkenv](https://github.com/thp/apkenv). Sibling project: the Palm
Pre→TouchPad `.ipk`-patching track for native webOS games.*
