---
name: android-apk-port-triage
description: "How to triage candidate games for a TouchPad/webOS port, and the slim-wrapper architecture for Android NDK games"
metadata: 
  node_type: memory
  type: project
  originSessionId: 7a813961-5d5f-46b3-b86d-ff6f66990032
  modified: 2026-09-14T15:14:21.441Z
---

Triage rubric for picking games to port to the HP TouchPad, plus the slim-wrapper plan for Android `.apk`s. Complements [[pre-to-touchpad-porting-playbook]] (which covers the surgical IPK-patch method for already-native games).

**Tier 1 — webOS PDK `.ipk` (proven, surgical path).** Always prefer this. Confirm with `readelf -d` on the inner ELF: want **GLIBC_2.x versioned symbols** + links to **libSDL/libpdl/libGLESv2**. Port = byte-patch for TouchPad shine (requiredMemory, MSAA/aniso, resolution). Many era games shipped a Palm App Catalog version alongside Android — check for it first.

**Tier 2 — Android `.apk` needing a compat wrapper.** Rank by wrapper thinness:
- Best: pure NDK NativeActivity games (`android.app.NativeActivity` in manifest, 1–2 `.so`s, minimal Java) — apkenv sweet spot.
- OK: Unity ~3.x/4.x NDK games (bounded JNI surface + Mono JIT wrinkle).
- Avoid: Dalvik-heavy (gameplay in classes.dex), Google Play Services / online-only / license-DRM, and **GLES3-only** (TouchPad Adreno 220 is **GLES2 only** — manifest `uses-feature glEsVersion` must be 0x00020000, not 0x00030000). Older minSdkVersion is better.

**2-min triage per apk:** unzip → count/size `lib/armeabi*/*.so` → manifest (NativeActivity vs Java Activity, glEsVersion, min/target SDK) → gameplay in .dex or native? → engine fingerprint (Unity version from `assets/bin/Data/mainData` header).

**Slim-wrapper architecture (Android NDK → webOS), proven shape = `apkenv`:** discard Dalvik + framework + window system (the heavy 80%); build only: (1) **bionic loader** to load ARMv7 `.so`s into a glibc process + resolve bionic libc shims — REUSE the ACL's loader here, the hardest piece; (2) **NDK C glue** — back `ANativeWindow` with a PDL/EGL window, feed touch into `AInputQueue`, point `AAssetManager` at on-disk assets (~30–40 documented fns); (3) **fake-JNI** for only the classes the engine calls; (4) **audio bridge** (FMOD AudioTrack JNI → webOS PCM). Make-or-break unknown = bionic-loader ↔ Mono-JIT (needs mmap PROT_EXEC). Spike first: dlopen libunity+libmono, reach `ANativeActivity_onCreate` with a black EGL window, before building the JNI shim.

See [[templerun2-port-analysis]] for the worked example numbers.

**CATALOG TRIAGE RESULTS (2026-06-28) — which `android-candidates/` are real native fits.** Confirmed by `readelf -d/-sW` on the engine `.so` + manifest. ✅=good native fit, ⚠️=native but hard, ❌=poor fit:
- ✅ **WheresMyWater** (libwmw, Walaber, GLES1, FMOD) — PLAYABLE (done).
- ⚠️ **WheresMyWater2** (libwalaber, Walaber, GLES1, FMOD) — boots/menu/audio/enters-level but **dead-ended on MULTI-THREADED GL** (loads textures on a worker thread; single GL context → deadlock; see [[wrapper-spike-progress]]). Generalization proven; not worth the GL-threading project as a catalog title.
- ⚠️ **BejeweledBlitz** (libBejBlitz, SexyAppFramework, GLES2+1, **OpenSL**, RegisterNatives, armeabi-only) — native but needs an OpenSL sink + dynamic-JNI RE.
- ⚠️ **FruitNinja** (libmortargame, Mortar, GLES2, FMOD+OpenSL, +MicroMap ad-JNI, 5MB dex) — native but GLES2 + ads + heavy Java.
- ❌ **TempleRun 1 (v1.6.1) AND TempleRun2** — BOTH **Unity + libmono** (logic in C# DLLs, Mono JIT). Imangi used Unity from the start; no native version exists. Poor fit (Mono runtime + Unity threading = worse than WMW2's wall).
- ⭐ **PvZ HD v1.1** (`PvZ HD v.1.1 ANDROID.apk`, 81MB, libpvz.so + PvZ.s3e + PvZ.dz) — **Airplay/Marmalade** engine (`com.ideaworks3d.airplay.*`), and **apkenv's existing `modules/marmalade.c` drives it** (matches AirplayThread/View/API). NATIVE, **self-contained** (1024x768 landscape = exact TouchPad match), tiny dex. **BOOTS TO A RENDERING MENU first try** (added marmalade.c to the webOS build). **Fixed touch delivery** (general): the engine's runNative blocks + only pumped SDL on glSwapBuffers, so a static menu idling via s3eDeviceYield dropped taps — now pump `input_update` on deviceYield too → all taps reach the engine (correct coords + `onMotionEvent(pointerId, androidAction+4, x, y)` encoding confirmed vs MultiTouch.smali). **Resource blocker SOLVED**: PvZ.dz is a **Marmalade Derbh** (DTRZ) archive the module never unpacks → built `apkenv/tools/derbh_extract.py` (byte-perfect, all 477 files; LZMA+zlib+store, unsorted offsets). Deployed the tree → resource-hunt loop GONE. **NOW: boots → renders a STABLE, live MENU; app threads run; touch reaches the engine — but the menu does NOT respond to taps.** Hard-won chain (my earlier "multi-threaded GL deadlock" guess was WRONG — corrected):
  - The post-menu **freeze was SUSPENDED APP-THREADS**, not a GL deadlock. Marmalade is a 2-thread model (engine thread ↔ UI thread); on Android `AirplayView.surfaceChanged` calls `resumeAppThreads` once the GL surface is ready. Our shim has no SurfaceView → never fired → app logic paused → menu frozen + taps ignored. FIX: spawn a real **OS/UI thread** (`marmalade_os_thread`) that drains `runOnOSTickNative` and calls **`resumeAppThreads`** (stall-triggered via `marm_swap_count`) → engine un-freezes (0→3000 GL ioctls/s).
  - The ~330MB RSS is a **fixed engine pool sized by the ICF `MemSize0`** (reads `…/.apkenv/<apk>/game.icf`); the game needs it (MemSize0=240000 OOM-died), so can't shrink. **Stability = device RAM headroom**: 381MB on the 918MB TouchPad is right at the edge — a **fresh `novacom reboot` + `/var/apkenv2/play-pvz.sh`** (frees background-service RAM) gives a stable, rendering instance. Thrashed/low-RAM device = the engine suspends + dies.
  - **OPEN (the real blocker): TAP PROPAGATION.** Taps reach `onMotionEvent` (MARMTOUCH confirmed, correct `(pointerId, androidAction+4, x,y)` encoding), app threads run, menu renders — yet NO menu reaction. Tried delivering onMotionEvent from BOTH the engine/render thread AND the OS/UI thread (touch queue) — neither works. **NOT coordinates** (user tapped many areas systematically, zero reaction — like the WMW1 "event-propagation thief").
    **>> NEXT SESSION (user directive): PULL THE s3ePointer INPUT THREAD.** Concrete plan: (1) `gdb-multiarch` is now installed + the working remote-attach recipe is in STAGE-5 (gdbserver `--attach 127.0.0.1:9001` on device + `ssh -N -L 9001:127.0.0.1:9001` tunnel + `add-symbol-file libpvz.so -o <r-xp base>`). (2) In libpvz find/break on the s3ePointer path — `s3ePointerUpdate`, `s3ePointerGetState`, `s3ePointerGetX/Y`, and any registered pointer **callback** — to see whether the menu/app actually POLLS pointer state each frame (if it never calls s3ePointerGetState/Update, the menu can't see taps → that's the thief). (3) Trace what the engine's `onMotionEvent` does with the touch (does it post into the s3ePointer queue? need a per-frame pump like the loaderthread runOnOSTick?). (4) Compare to MultiTouch.smali/AirplayThread touch flow for any step we skip. Launch for testing: fresh `novacom reboot` first (memory headroom), then `/var/apkenv2/play-pvz.sh`.
  **Furthest a NEW-engine (Marmalade) candidate got.** Full trail: `plan/STAGE-5` pt5-7 + commits. ❌ **PvZ2** and ❌ **pvzfree (EA)** are freemium/ads sequels (downloaded assets, online).
- ❌ **PlantsVsZombies2** (libPVZ2, EA EAIO/EAThread, GLES2, **OpenSL**, RegisterNatives) — native engine BUT **freemium sequel**: multi-threaded (EAThread → the WMW2 GL wall), OpenSL, and **game assets are NOT in the apk** (6MB apk, downloaded online on first run). Showstopper for an offline shim.
- ❌ **CutTheRope** (7MB dex, Dalvik-heavy), **FlappyBird** (AndEngine, all-Java) — logic in Dalvik, not native. Skip.
- ✅⭐ **Dead Space** (`Dead-Space.apk`, 2026-09-14 triage, static only) — `com.eamobile.deadspace_full_azn` 1.2.0,
  **Amazon/Kindle-Fire build** (compatible-screens 400/160 = 1024x600). **EA "Blast" framework** (new family, no
  module). One `libDeadSpace.so` (6.7MB, armv5TE), NEEDED = libc/stdc++/m/log/**GLESv1_CM only** (manifest says
  0x20000 but engine is ES1 + OES FBO/blend ext — all in `gles_mapping.h`). **No EGL imports** → Java owns the only
  context → no WMW2 wall. Engine→Java contract **tiny**: ~45 value-returning device getters (GetDefaultWidth, GetDpiX,
  GetTotalRAM…), Startup/Shutdown, SetStdOrientation, IntentView; only gate = `Query.isContentReady()Z` (static flag →
  return true). **Textures all uncompressed** M3G Image2D (RGBA/RGB 120-B header, or 16bpp+mips 134-B) — verified by
  size arithmetic over every file + a rendered 1024² atlas. **All content in apk** (`assets/published`, 320MB, mostly
  Deflate) — `loadContent()` takes the no-download branch; Kiwi DRM + eamobile licensing are Java-only → never run.
  Engine reads assets via **Java AssetManager over JNI** (open/list/openFd/read/skip/close/getLength) — apkenv has no
  generic one, must write. Audio = `AndroidEAAudioCore.Init(AudioTrack,III)` → engine **pushes** `AudioTrack.write([SII)I`.
  Landscape (setRequestedOrientation 0). EA Job Manager threads (CPU jobs; `-singlethreaded` string exists). DirtySDK
  net code (offline-fail expected). Blast order: `NativeOnCreate` → GLSurfaceView `NativeOnSurfaceCreated/Changed/DrawFrame`.
- ✅ **Fruit Ninja 2.1.2** (`04 Fruit Ninja 2.1.2 (201201)/`, apk + 56MB **OBB**, 2026-09-14 triage) — Mortar, GLES2
  (TR2 proved ES2), no EGL imports. Textures **uncompressed RGBA** (`.tex` fmt byte 1, 501/502; DXT strings are an
  unused option). Assets in OBB (zip, store+deflate) that the **engine opens itself**: it builds
  `<externalDir>/Android/obb/<pkg>/main.<ver>.<pkg>.obb` (strings `/Android/obb/`, `.obb`; own zlib).
  `InitFileManager(apkPath, saveDir, cacheDir, externalDir, Z)`. **Upstream `modules/fruitninja.c` is for 1.5.4/1.7.6
  and the API moved** (now `SystemInit(IILString)`+`GameInit()`, `touchEvent(IJIFFFF)`, engine-side mixer) → rewrite,
  and it's not in `build-webos.sh`. Audio: `SupportsOpenSL()`→false selects `MortarAudioMixerOut` = engine **pushes**
  `WriteData([S)` → **same push-AudioTrack sink as Dead Space (build once)**. Must host `KeyStore.GetValue/SetValue`
  (Java-side persistent save store: `KeyStore.dat` + prefs, `~~rev`) — like TR2 PlayerPrefs. Big stub surface: ~150
  engine→Java names (AdMob/MobClix/Domob/Vungle, Bricknet Facebook/Google/Twitter/WebView/Billing, push, location);
  blocking-capable to rank: MortarDialog show/createDialog, HttpRequest, LoadHTML/WebView, Login, InitiatePurchase.
  Catalog's 1.8.8 is the same API generation + libMicroMapJNI → not simpler.
- ✅ **Aralon: Sword and Shadow HD 4.53** (`Aralon/`, 2026-09-14 triage, static only) — **Unity 4.0.1f2 + Mono**,
  same proxy layout as TR2 (real engine in `assets/libs/armeabi-v7a/`). **Reuses the TR2 stack almost whole:**
  (1) every one of libunity's 119 Mono imports is exported by our `hostlibs/webos/libmono-webos.so`; Aralon's bionic
  Mono has 5 extra exports but libunity never names them; **corlib version = 82 in Aralon's Mono, TR2's Mono AND our
  host Mono** (read from `mono_check_corlib_version`'s `cmp #82`), so Aralon's own (different) mscorlib passes the check;
  (2) `org/fmod/FMODAudioDevice.smali` **byte-identical** to TR2's → same pump (and the same music-stream-stall RISK;
  TR2's single-bed workaround is weak for a per-area RPG soundtrack); (3) UnityPlayer natives identical except
  `unityAndroidInit` now returns **Z** and new `nativeSetInputCanceled`; `nativeInit(II)` unchanged.
  Contract delta vs TR2: +6 non-blocking names (getScreenOrientationAngle, getCameraOrientation, vibrationSupported,
  enable/isSensorCompensationEnabled, setSoftInputStr) — none in unity.c yet. **OBB** (`settings.xml useObb=True`):
  286MB zip → 512MB `assets/bin/Data/*` (mostly Deflate, `.resS` Stored); Java builds
  `<ext>/Android/obb/<pkg>/main.10.<pkg>.obb` and passes it via a 2nd `nativeFile()` → unity.c needs that 2nd call.
  Landscape (sensorLandscape) → no rotation. GLES2, no supports-gl-texture (Unity default ETC1 → native on ES2).
  Multi-touch plumbing exists (webos.c finger 0..4 → unity.c pointerId) but TR2 only ever exercised 1 finger — check
  ACTION_POINTER_DOWN semantics for stick+camera. **The `-mod` apk is an IAP crack only** (prime31 IABPlugin +
  IInAppBillingService + firstpass.dll); IAPs are 6 consumable "karma" packs → **use the pristine original**.
  **PORT STATUS 2026-09-14 (same day): boots, renders, menu + touch work, New Game → cutscene → character
  creation; NO AUDIO yet.** Trail: `plan/ARALON.md` (runs A1–A4). Three Unity-4 host-contract gaps found, each
  statically: (1) Mono bridge list was TR2's → now union of per-game `plan/*-mono-imports.txt`; (2) Unity 4 needs
  `nativeResize` (onSurfaceChanged) + one `nativeRender` BEFORE `unityAndroidInit` — pre-init nativeResize is what
  builds the loading blitter; (3) managed AndroidJavaObject needs ReflectionHelper + FromReflected* + A-variants +
  NewGlobalRef identity — Aralon's GuiMgr sizes the GUI from DisplayMetrics (0x0 → invisible menu, dead taps).
  All Unity-4-gated in `modules/unity.c` (TR2 path untouched, no regression run yet).
  **MUSIC FIXED same day (A14):** music was silent while SFX worked because FMOD's file thread (8 KB
  bionic-sized stack) could not be created under glibc (min 16 KB → EINVAL → FMOD_ERR_INTERNAL 33 →
  Unity silently kept a 0-length "ready" clip). `APKENV_PTHREAD_STACK_CLAMP=1` raises small stack
  requests to a 128 KB floor (16 KB was still too small: glibc descriptor + apkenv trampoline's
  unbuffered-stderr fprintf share the stack). Systemic — likely relevant to any bionic engine that
  sizes thread stacks small. Found with `APKENV_UNITY_ICALL_TRACE=1` (compat/icall_trace.c: wraps the
  bridged `mono_add_internal_call` to trace audio icalls) + an Android reference tablet (HP 10 G2) via
  adb. Trail: `plan/ARALON.md` A1–A14.
  **Graphics PARKED by the user (A15–A16):** outdoors looks slightly darker/less warm than on a Mali
  Android tablet (house lights cast less coloured glow). Not uploads (0 GL failures in 6000), not alpha
  compositing (opaque present: no change), no Adreno-keyed Unity workaround (no GPU strings in
  libunity). User: probably just screen differences — don't chase it. Lead kept in the plan doc.
  **RELEASED 1.0.0 (2026-09-14)**, fresh-install verified on the TouchPad:
  `apkenv/packaging/out/com.apkenv.aralon_1.0.0_all.ipk`. Release env = HOST_MONO, UNITY_PORTRAIT=0,
  UNITY_SPLASH=0, UNITY_PACKAGE, UNITY_OBB, PTHREAD_STACK_CLAMP=1. User's pre-release save moved to
  `/media/internal/.apkenv/aralon.apk.bak-20260914-110225`.

**KEY PATTERN: SEQUELS are the complex ones; get the ORIGINALS.** Across TempleRun, WMW, PvZ the sequel went Unity/freemium/online/threaded while the original is native+offline+simpler. Always hunt the **oldest, paid** version (free/EA re-releases add online/store/downloaded-assets). **Next target: PvZ *1*** (original PopCap SexyAppFramework, fully offline) — pending the user finding an early paid build.

**New hard-won filters (beyond the rubric above):** (a) **does it thread its asset load + touch GL?** — the WMW2 killer; a single-context shim deadlocks. Check `pthread_create`/`EAThread`/`*LoaderThread` + GLES use. (b) **are the game assets actually IN the apk?** — freemium titles ship a tiny apk and download content (PvZ2); unrunnable offline. (c) **audio backend**: FMOD-AudioTrack reuses our pump; **OpenSL/OpenAL needs a new sink**. (d) **WHERE DO RESOURCES LIVE? — budget an UNPACK step.** Bitten TWICE (WMW + PvZ HD): if a game packs resources in a custom archive (PvZ HD = `assets/PvZ.dz`, a **DTRZ** archive: magic `DTRZ`, u16 count + null-terminated filename table + offset/size table + zlib-DEFLATE `78 da` blobs), **streaming them through the shim at runtime is UNSTABLE** — menu limps but level loads spin (strace = looping `open()` on an empty `…/.apkenv/<apk>/compiled/`). Cure every time: **unpack the archive host-side in advance**, push the tree, redirect the engine's `fopen`/`open` to real files. `modules/marmalade.c` has `// TODO: extract files (implement dzip algorithm)` — never implemented. Full lesson: `android-port-shim.md` §8.

**STEP ZERO gate (2026-08-31) — run before anything else, incl. the manifest read:**
`unzip -l cand.apk | grep -E 'lib/.*\.so'` → **no hit = no port on the APKENV track.** (NOT "impossible" —
see the Java-track note below; Java bytecode is ISA-neutral, so this gate is about our MECHANISM, not the CPU.) apkenv is
bionic-loader + *fake*-JNI: it runs native code and synthesizes the Java host. An all-Java game is the
exact inverse — nothing to load, nothing to fake. Only a real Dalvik VM + framework (= ACL, out of
scope) runs it. Costs 2 seconds and short-circuits an otherwise promising-looking profile.

- ❌ **Space Cat / Space Cat HQ (thepilltree)** — BOTH versions checked 2026-08-31, both **pure Java,
  ZERO `.so`**, permanently out of scope. `com.thepilltree.spacecat_1.0.3` (2011, 8.3MB, dex 946
  classes, minSdk 4) and `com.thepilltree.spacecathd--120` v2.0.6 (2014, 12MB, dex 3311 classes,
  minSdk 9). Engine both times = **jPCT-AE** (`com.threed.jpct.*`, ~80 classes) — a **100%-Java GLES2
  engine** on GLSurfaceView. **Fingerprint it instantly:** `defaultVertexShader.src` /
  `defaultFragmentShader.src` sit at the APK ROOT and the vertex shader carries the literal comment
  `"in jPCT-AE, it always is"`. jPCT's lone `loadLibrary` is its optional `BufferUtilNative`
  accelerator, which ships no `.so` and falls back to pure-Java `BufferUtilVM` — **a `loadLibrary`
  string is NOT evidence of native code.** Also checked for the TempleRun2 `assets/libs/` hide — absent.
  Everything else about them was ideal (minSdk 4/9 = the Gingerbread contract we already host; native
  **landscape**, so no FBO-rotation work; 12MB, no OBB; GLES2 fits Adreno 220) — which is exactly why
  the gate must run FIRST. **This studio is a Java house; no version of this title will ever qualify.**
  So the "hunt the oldest paid version" pattern has a limit: it beats freemium/online/threading
  regressions, but it can NOT undo an all-Java engine choice — that was never a regression to reverse.

**JAVA TRACK — measured, NOT dismissed (2026-08-31).** User pushed back correctly: "if it's Java it's
not really CPU-specific — can't we crack it and run the interpreted code anywhere?" Yes. Measured the
real `android.*` surface by parsing dex code items and attributing every `invoke-*` to its CALLING class
(`scratchpad/attrib.py` recipe: class_defs → class_data → code_item → opcodes 0x6e-0x78 → method_ids):
- **Core (game + jPCT-AE only) is TINY and STABLE:** 337 methods/63 classes (2011) vs 352/68 (2014) —
  3 years apart, barely moved, because jPCT-AE is the constant. Of those, **~151 are GL**
  (GLES20 79 + GL10 54 + GL11 11 + Matrix 7) which webOS already has → **non-GL core ≈ 190 methods /
  ~55 classes.**
- **Everything else is strippable, none of it gameplay:** Scoreloop 293, PayPal 174, Facebook 65,
  Ads 55 (2011); PlayServices 335, Ads 244, Billing 49, Gamepad 49 (2014).
- Most of the ~55 classes map to subsystems **we already built**: GLSurfaceView→PDL/EGL,
  MotionEvent→PDL touch, SensorManager→PDL sensors (TR2), MediaPlayer/SoundPool→our audio pump,
  SharedPreferences→PlayerPrefs-style properties file. Extra work: `resources.arsc` + LayoutInflater
  (69 `res/layout/*.xml` for its dialogs) and Bitmap decode.
- **The one HIGH-RISK unknown is the JVM, and it must be JIT'd** — jPCT-AE does scene graph + matrix
  math in Java every frame; a pure interpreter (Zero/JamVM-no-JIT) likely won't hold framerate.
  Encouraging: Gingerbread Dalvik's JIT was weak and this shipped on 2011 phones, while TouchPad's
  dual-1.2GHz APQ8060 is stronger than a typical 2011 handset.
- **DE-RISK ON DESKTOP FIRST** (per [[systematic-not-brute-force]]): dex2jar/enjarify the apk, shim
  `android.*` over LWJGL on a PC, see if it runs AT ALL. Validates the conversion + the shim with zero
  webOS risk, before gambling on the JVM port. Also check whether jPCT-AE's desktop sibling (jPCT
  proper, LWJGL/JOGL) is API-parallel enough to SUBSTITUTE rather than shim GLES.
- **Why this is strategically interesting:** unlike apkenv's bespoke per-game modules, this work is
  **per-platform, written once** — it would unlock the whole all-Java segment currently marked dead:
  FlappyBird (AndEngine), CutTheRope (7MB dex), both Space Cats, and the large jPCT-AE/AndEngine/libGDX
  catalogs. Counterweight: plausibly a bigger project than every native port so far combined.
- ✅ **Tiny Death Star** (2026-09-16 triage, static only) — two apks: `tiny-deathstar_1.4.2.apk` is really
  **1.4.1** (versionCode 16; filename is wrong) and `Star_Wars__Tiny_Death_Star_1.2.2_Android_2.3.3.apk` (vc 13).
  Both are the same host generation: **Cocos2d-x 2.x** (new family, no module) in `libgame.so` (armv7, ES2, **no EGL
  imports**), the same 21 JNI exports, and the same ~50 native→Java statics. **Prefer 1.4.1** (more content, no
  contract delta). Portrait. Assets are loose in apk `assets/` (PNG RGBA + plist/json; the engine reads the apk
  itself via `nativeSetApkPath`). New host work: (1) **text** — `Cocos2dxBitmap.createTextBitmap` → Java Canvas;
  rasterize with the PDK's **SDL_ttf/freetype** from the apk's own TTFs, then `nativeInitBitmapDC(II[B)`;
  (2) **audio** — FMOD Ex + Event (`.fev`/`.fsb`), but this libfmodex has **OpenSL output only** (no
  `org/fmod` class in the dex, no AudioTrack output) → needs an **OpenSL ES shim** (also unblocks Bejeweled).
  Online stack (Playdom MSDK native sockets, DMO IAP, Tapjoy, UrbanAirship) → `isOnline()`=false, stub the rest.
  **→ PORTED 2026-09-16 in one session** (`modules/cocos2dx.c`, RC 1.0.0; see `plan/TINY-DEATH-STAR.md`). Correction to this triage: that libfmodex DOES also have an AudioTrack output (autodetect: OpenSL if libOpenSLES dlopens, else AudioTrack); the OpenSL shim was built anyway and works.
- ⚠️ **RoboCop 3.0.6** (Glu, `robocop_3.0.6.apk` + `main.1309.com.glu.robocop.obb`, 2026-09-16 scoping, NOT started) —
  **Unity 4.2.2f1** + Mono; the apk's libunity/libmono are 43-92 KB proxies, and the real engine (9.5 MB) + libmono
  are in the OBB `assets/libs/`. 299 MB OBB (zip), landscape, ES2. Closest prior port = Aralon (Unity 4.0.1, OBB).
  Gaps: (1) the **host Mono is too old** — libunity imports 6 `mono_unity_*` symbols (class_is_abstract/interface,
  liveness_calculation_*) that `hostlibs/webos/libmono-webos.so` lacks → rebuild from Unity's 4.2-era mono branch;
  (2) a heavy Glu plugin layer (~50 Java classes named in C#: AJavaTools/AJTDeviceInfo/AJTInternet, AAds, ASocial,
  in-app purchase) for the AndroidJavaObject reflection bridge to answer; (3) online risk: strings include "Unable
  to connect to the server... internet connection is required", but **the user confirmed (2026-09-16) it runs
  offline under ACL on the TouchPad** — so no hard online gate, and ACL is a same-device reference oracle for it; (4) launcher = OBB DownloaderActivity (LVL) → UnityPlayerNativeActivity, but libunity
  also RegisterNatives the Java path (nativeTouch/nativeRender), so unity.c's model should apply; (5) texture
  compression format not yet checked. 7.4 MB dex (ads/analytics SDKs, not gameplay). Packaging precedent = **Aralon's OBB path** (EXTRAS + APKENV_UNITY_OBB), NOT Dead Space (DS had no OBB — content was inside the apk, staged as files). Full scope: `plan/ROBOCOP-SCOPING.md`.
