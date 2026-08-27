# PORTING-PLAYBOOK.md — how to bring the *next* Android NDK game to webOS

Distilled from two shipped ports (Where's My Water?, Plants vs. Zombies HD) and one that stalled
(WMW2). `android-port-shim.md` is the architecture field guide; this is the **method** — what to
do, in what order, and the traps that cost whole sessions. Read this before touching a device.

## 0. The one rule

**The game never talks to webOS. It talks to its Android Java host, and apkenv's per-engine module
*is* that host.** Almost every "mysterious freeze / silent feature" we hit was a Java method the
engine called that the module didn't implement (fake-JNI returns 0/NULL silently). So:

> Derive the host contract statically first, diff it against the module, and treat every
> unimplemented *blocking-capable* call as a ranked theory — **before** any deploy/tap loop.

PvZ HD cost days of brute-force patching (fibre schedulers, free-RAM gates, thread resumes) and was
fixed in one afternoon by this method: the engine was spinning in `s3eOSReadString` waiting for the
Android name-entry dialog (`getInputString`) we never answered.

## 1. Triage a candidate (30 min, no device)

```
unzip -oq game.apk -d apk && ls apk/lib/*/ apk/assets | head
baksmali d apk/classes.dex -o smali      # the Java host
strings -n 5 apk/lib/armeabi/lib*.so | grep -iE 'jni|s3e|fmod|opensl|unity|mono|cocos'
```
Decide: engine family (Walaber / Marmalade-Airplay / Unity+Mono / cocos2d…), GL version, audio path
(FMOD / OpenSL / AudioTrack pump / MediaPlayer), how much of the game is Java (Dalvik-heavy = poor
fit). Existing modules: `apkenv/modules/wheresmywater.c` (Walaber), `marmalade.c` (Marmalade AND
Airplay 4.x), plus upstream apkenv modules for other engines.

**Know which binary is the game.** Marmalade/Airplay apps ship the *runtime* as `lib<name>.so` and
the actual game as `assets/<name>.s3e` (LZMA-Alone; `python3 -c 'import lzma…'`). Reverse-engineering
the runtime for game behaviour is a dead end (we did it for a week).

## 2. Derive the host contract (1–2 h, no device)

1. List everything the engine can call **into Java**:
   ```
   grep -rhoE '^\.method[^(]* [a-zA-Z]+\(' smali/<host pkg> | awk '{print $NF}' | tr -d '(' | sort -u > javanames
   grep -xF -f javanames <(strings -n4 lib.so) | sort -u          # names the .so actually references
   ```
2. List what the module handles: `grep -oE 'method_is\([a-zA-Z]+\)' modules/<engine>.c`.
3. For every unhandled name, read its smali body and classify:
   - **blocking-capable** (posts a dialog, waits on a flag, is polled by the engine) → a theory;
   - value-returning (status, volume, orientation, sizes) → likely a silent-feature bug;
   - fire-and-forget → ignore.
4. Confirm the engine side: find the string's literal-pool xref in the disassembly
   (`objdump -d -M force-thumb`; search the ELF for the little-endian address of the name string)
   and read the loop after the JNI call. A `while (slot == 0) deviceYield()` there *is* the freeze.
5. **Check JNI signatures per host generation.** Same method name ≠ same args: Airplay's
   `audioPlay(String,int)` vs Marmalade's `(String,IJJI)`, `soundInit(ZI)` vs `(IZI)`. Reading a
   5-arg va_list off a 2-arg call silently yields garbage. Dispatch on `method->sig`.

The Java host also documents *semantics* you must reproduce: e.g. Airplay `audioPlay(repeats=0)`
means **loop forever**; `soundInit(rate=0)` means "native rate" (Android returns 44100);
`deviceYield(ms<0)` blocks until `deviceUnYield`.

## 3. Instrument before you test

Every module should ship an always-on **unhandled call-out tracer** (`marm_trace_unhandled` in
`marmalade.c` is the template): print each unimplemented engine→host call once with its signature,
then at 100/10k/1M. With that in place, the *next* gap names itself in the device log instead of
presenting as a freeze. Add one log line per implemented contract point (`[OSREADSTRING]`,
`[MARM-AUDIO] audioPlay '<path>' -> rc`, `[MARM-SOUND] soundInit …`, `[MARM-LB] …`) so a test
run answers "did the fix fire, and what came next" without a second round-trip.

Prepare the test as a protocol: one change, what line to expect, what the user should see, what
the fallback signal is if it fails. The person holding the device is the scarce resource.

## 4. Subsystem lessons (all general, all in the tree)

**Input.** `PDL_Init` before `SDL_Init`; feed engines what their Java `onTouchEvent` fed them
(WMW: normalized 0..1; Marmalade: `onMotionEvent(id, action+4, x, y)` pixels). Pump SDL input on
*every* engine yield, not only on swap — static menus idle without rendering and drop taps.

**Fake-JNI correctness (three bugs that cost a session each).**
- **Override the `V` variant of every `Call*Method` you override.** A module that overrides
  `CallObjectMethod` but not `CallObjectMethodV` leaves the va_list form on `jni/jnienv.c`'s
  generic fallback, which returns the `GLOBAL_J(env)` **sentinel** for any unanswered object
  call. The engine then feeds that to `GetStringUTFChars`, gets NULL, and builds a
  `std::string` from it — `SIGSEGV` in `strlen` with no clue which host method was wanted.
- **`GetStringUTFChars` must return a COPY.** `ReleaseStringUTFChars` `free()`s whatever Get
  returned, so handing back the jstring's own buffer frees it; the next Get on that jstring
  aborts the process with `double free or corruption (fasttop)`. Bit WMW2, fixed in
  `jni/jnienv.c`, then bit Temple Run 2 again through a module override.
- **An unimplemented host method that *returns a value the engine acts on* is not a no-op.**
  Unity's managed `PlayerPrefs.SetX()` **throws** when the Java side returns false, killing the
  caller's `Awake()`. Returning 0/NULL "harmlessly" is how you lose a game's startup objects.

**Hosting a subsystem means you can READ it — the game's own settings are an input you already
have.** Temple Run 2's music bed needed to follow the in-game music slider. No disassembly was
required: the game stores that setting through *our* PlayerPrefs, so `novacom get` on the device's
`playerprefs.txt` named the key (`TR Music Volume`) in one step, and a two-line hook on `SetFloat`
drives the mixer live (`APKENV_FMOD_MUSIC_PREF`). Cross-check the value against the game's own save
file if it keeps one, to be sure it is the setting and not an internal scalar. Before reverse-
engineering a value out of an engine, look in the stores you already host: prefs, save files,
the data dir.

**Input focus comes from the launcher, not from novacom.** A binary started with
`novacom run` renders fine but receives **no SDL input at all** — `ev_total=1` over 2400 polls
and, tellingly, **no `SDL_ACTIVEEVENT`**. Installed and launched from its icon, the same binary
gets a normal stream of `SDL_MOUSEBUTTONDOWN`/`UP`. Foreground vs detached makes no difference.
**Test touch from a packaged `.ipk`**, and do not spend time debugging "dead input" until you have.

**Ask the engine for the orientation; don't infer it from the screen.** Temple Run 2 *looks*
like a portrait game and is one on phones, but this build calls `setOrientation(0)` =
`SCREEN_ORIENTATION_LANDSCAPE` and its manifest declares none. Handling `setOrientation` as a
logged no-op is cheap and settles the question before anyone builds a rotation path.

**Dialogs / blocking host calls.** Text entry (`getInputString` → native `setInputText`), error
boxes (`showError`), anything modal: answer them synchronously from the module (check the engine
clears its result slot *before* the call — then an in-call answer is race-free). Never answer
empty strings where the game re-prompts.

**Audio (three shapes seen).**
- FMOD → `audio/fmod_pump.c` (AudioTrack-style pull pump; WMW).
- Marmalade `SoundPlayer.generateAudio(short[], nFrames)` → mixer **post-mix hook**
  (`apkenv_mixer_set_postmix`, SDL `Mix_SetPostMix`); `nFrames` is per-channel; widen mono→stereo.
- MediaPlayer music (`audioPlay(path)`) → SDL_mixer music. Device facts: the TouchPad's
  `libSDL_mixer` has vorbis **and** an ffmpeg MP3 decoder but **will not resample or remix music** —
  open the mixer 44100 Hz stereo and ship music at exactly that (`ffmpeg -ac 2 -ar 44100 -c:a
  libvorbis`). Paths arrive relative (Android cwd is `/`): try `/`+path and an `.ogg` twin.
  Make `load_music` return NULL on failure (upstream returned an empty wrapper = silent success).

**GL wrappers: never register two tables that share names.** `gles_mapping.h` and
`gles2_mapping.h` share **68** symbols (`glClear`, `glDrawArrays`, `glViewport`,
`glBindTexture`, ...). An engine that links both GL libs makes apkenv register both tables;
appending duplicates leaves `apkenv_get_hooked_symbol()` `bsearch`ing a table with two entries
per name and taking an arbitrary one, so the engine's calls split across two wrappers against a
single context. Symptom: correct viewport, ~10k draw calls and 14M vertices a second, and a
**blank screen**. `register_hooks_nodup()` keeps the first registration (DT_NEEDED order puts
`libGLESv1_CM.so` first). *Corollary for debugging: a probe that instruments only one of the
two tables will confidently report "0 draws". Instrument every path a symbol can take.*

**Compressed textures.** Android games ship **ETC1** as a matter of course, and the TouchPad's
GLES1 context does **not** expose `GL_OES_compressed_ETC1_RGB8_texture` —
`glCompressedTexImage2D` returns `GL_INVALID_ENUM` and every upload is silently dropped, so
geometry draws untextured. `compat/etc1.c` decodes it on the CPU (unit test:
`tools/etc1test.c`). Note ETC1 carries **no alpha**; Unity splits alpha into a second ETC1
texture, so an RGB-only upload is not the whole story for anything blended.

**Display.** Landscape-native TouchPad (1024x768). Portrait games: render-to-FBO + one rotated
blit (WMW). Wrong aspect (PvZ ships only 1280x800 assets): `APKENV_MARM_LOGICAL=WxH` reports a
centered surface of that aspect; generic `module_hacks.viewport_offset_{x,y}` shifts every
real-framebuffer `glViewport`/`glScissor`, touches are shifted the same, and
`apkenv_gles_clear_screen()` after each swap paints the bars (unpainted rows show the launcher
wallpaper through the compositor — that was the "thin strip at the top").

**Threads.** Before theorising about worker threads, read who calls `suspendAppThreads`-style
natives in the Java host. In Airplay it is the *UI thread* parking itself during surface changes;
"resume app threads" fixes built on the opposite reading did nothing. A `futex_wait` thread at a
freeze is usually SDL audio.

**Resources.** Unpack archives in advance (Derbh: `tools/derbh_extract.py`), never stream in place.
Memory: PvZ needs ~450 MB free; `requiredMemory` in `appinfo.json` makes webOS reclaim before launch.

## 5. Build / deploy / package (the mechanics)

- Build: `apkenv/build-webos.sh` (two toolchains, see `android-port-shim.md` §2). **No header
  dependency tracking** — it rebuilds an object only when the `.c` is newer than the `.o`. `rm
  build/webos/*.o` (or `touch` the `.c`) after touching any shared header (`apkenv.h`, `mixer.h`)
  **or any generated one** (`compat/mono_symbols.h`), or you get a stale-struct assert on device —
  or, worse, a silently stale table: regenerating the Mono symbol list alone relinked the old one
  and the device log kept reporting the previous count.
- Transport: **USB/novacom is the reliable path** (`novacom put file:///path < local`,
  `novacom run file:///bin/<binary> -- args`; `sh -c` argument passing is mangled — run binaries
  directly, or a script file via `novacom run file:///bin/sh -- /path/script.sh`). SSH `.88` works
  only with the legacy-cipher options in `tools/deploy-tp.sh`. Three traps, each cost a cycle:
  **(1)** pass the `--` separator exactly once. A second one is delivered as the target's `argv[1]`,
  and busybox then treats the real flags as operands — it surfaced as
  `mkdir: can't create directory '-p'`. **(2)** `novacom run`'s cwd is `/`, which webOS mounts
  **read-only** (`/var` and `/media/internal` are rw), so a mis-parsed relative path fails with a
  confusing `Read-only file system`. **(3)** A hung `novacom run` **wedges the host daemon** — every
  later `novacom -l` then times out forever, even after killing the client. Recover with
  `sudo systemctl restart novacomd`. Always run device commands under `timeout`, and run long-lived
  GUI binaries detached device-side (`( ./apkenv … & ); sleep N; killall apkenv`) so the session
  always returns.
- Dev harness: `/var/apkenv2/{apkenv,libs/webos,play-*.sh}` + apk on `/media/internal` + data tree
  in `/media/internal/.apkenv/<apk>/`; log to `/media/internal/apkenv-<name>.log`. A re-flashed
  device loses `/var` — rebuild the harness from the installed WMW `.ipk`'s `libs/webos`.
- Package: `packaging/build-ipk.sh` with `APPID/APK/APPINFO` and the per-game extras
  `DATA=<tree>` (shipped as `android/<apk>.data/`, seeded once into `/media/internal/.apkenv/<apk>/`
  by apkenv — FAT has no symlinks, the PDK jail has no tar) and `ENVFILE=` (`android/apkenv.env`,
  `KEY=VALUE` lines `setenv`'d at packaged launch; there is no shell in the jail). Per-game inputs
  live in `packaging/<game>/`. Install with `palm-install`; a cut-off install leaves a half app dir
  that makes the next install fail `FAILED_PACKAGEFILE_NOT_FOUND` — `rm -rf` it first.
- Packaged log: `/media/internal/apkenv-<appid>.log`.

**Worked example of the method (Amazing Alex HD, 2026-08-26):** triage → ka3d = `angrybirds.c`;
contract diff found four gaps (zip-wrapped `readFile`, 3-arg key input, missing post-init
`nativeResize`, `getUniqueId`) and one build omission; fixed statically; **booted with music on the
first device launch**. ~1 hour. `plan/AMAZING-ALEX.md`.

**A harder class — the engine brings its own runtime (Unity/Mono; Temple Run 2).** When the apk
ships a JIT + GC (Mono, and anything like it) the failures move *below* the Java contract into
**bionic↔glibc ABI mismatches**: unhooked allocator entry points (a second heap), struct layouts
(`sigaction`, `sigset_t`, `pthread_attr_t`), fatal stubs for bionic-private pthread calls. Symptoms
are memory corruption, not clean errors, and the contract-derivation method above stops applying.

> **Don't translate the ABI call-by-call — replace the runtime.** Build the *same* runtime against
> the device's glibc and bridge the engine's imports to it. That deletes the entire corruption class
> in one step instead of chasing it symbol by symbol.

This is now general infrastructure, not a Temple Run 2 hack:

- **`compat/hostlib.[ch]` — the host-library bridge.** `apkenv_hostlib_bridge(path, soname, syms, n)`
  `dlopen()`s a *glibc* `.so` and registers its symbols as hooks. It works because the bionic linker
  consults the hook table **before** library symbols for every relocation (`linker.c:1368`), so the
  hooks shadow the apk's own copy. Pair it with (a) an entry in `builtin_libs[]` so `DT_NEEDED`
  resolves without a file, and (b) a `libblacklist[]` entry so the apk's bionic copy never loads.
  Both are gated on a host lib actually claiming that SONAME, so other ports are untouched.
  Opt in per run: `APKENV_HOST_MONO=<path>`.
- **Host libs live in `hostlibs/<platform>/`, never `libs/<platform>/`.** The latter is
  `APKENV_LOCAL_BIONIC_PATH` — the *bionic* linker's search path — and `packaging/build-ipk.sh`
  copies `*.so` out of it into the jail's bionic lib dir. A glibc object there is a loaded gun.
- **Derive the bridge set as {engine's UNDEFINED symbols} ∩ {runtime's DEFINED symbols}.** Do *not*
  match a name prefix. Unity's libunity imports 118 `mono_*` **plus** `g_free` (Mono's embedded
  eglib) and `GC_delete_thread`/`GC_lookup_thread` (Boehm) — 121 in total; the prefix filter drops
  three and the run dies at `cannot locate 'g_free'... failed to link libunity.so`.
  `tools/gen-mono-hooklist.sh` does the intersection.
- **Verify the pin by comparing export sets.** Our glibc Mono and the apk's bionic Mono export the
  *same 910 symbols, zero difference either way* — that is what proves you checked out the exact
  tree the game shipped, far better than matching a version string.
- **`RTLD_GLOBAL` is safe here but check it:** the bridged runtime publishes every export globally.
  Confirm none collide with libc (`malloc/free/read/write/mmap/sigaction/pthread_create/dlopen`).
- **Cross-building an old runtime with the PalmPDK toolchain** (general, will recur): glibc 2.5
  headers + gcc 4.3 `-std=gnu99` + any `-D_FORTIFY_SOURCE=2` ⇒ `multiple definition of
  realpath/fgets/gets/stpncpy/...`, because `bits/stdio2.h` & friends use a bare
  `extern __always_inline`, which under C99 emits an external definition in **every** TU.
  Fix: **`-fgnu89-inline`**. Also expect `AUTOMAKE_OPTIONS = cygnus` (removed in automake 1.13+) in
  any pre-2010 tree. Look for the vendor's own build script in the source drop first
  (Unity shipped `build_runtime_android.sh`) — it is the authoritative flag set; strip its
  Android-specific defines and keep the CPU/ABI flags. Cross builds cannot run `AC_TRY_RUN` tests,
  so their cache vars must be passed explicitly (`mono_cv_uscore=no` for ELF/glibc — Unity's `yes`
  is a bionic quirk that would break `__Internal` P/Invokes).

**Result:** the bionic Mono died inside `mono_jit_init_version` with a corrupted PC. The native one
initialises, loads `mscorlib` into the Unity Root Domain, JITs and runs managed code on the
TouchPad — and the remaining work went straight back to being ordinary Java-contract work.
`plan/TEMPLERUN2-MONO.md` (method + full trail), `plan/TEMPLERUN2.md` (history).

## 6. Checklist for the next game

- [ ] Triage (engine, GL, audio, Java share); identify the *game* binary vs the *runtime*.
- [ ] Host contract table: engine→Java names vs module handlers; blocking-capable gaps ranked.
- [ ] Signature check per host generation for every shared method name.
- [ ] Tracer + per-contract log lines in the module; test protocol written (expected lines).
- [ ] Input pump on yield; dialogs answered; audio shape identified; display aspect declared.
- [ ] Data tree extracted + music transcoded 44100/stereo; `packaging/<game>/` (appinfo, env).
- [ ] **Whose job list:** what did the ANDROID PLATFORM do that we now don't? Splash/window over the
      load, settings the player expects to work, lifecycle callbacks. Each is ours to supply.
- [ ] **Settings we already host:** dump the device's prefs/save files and wire the ones the player
      can see (music/sound volume) to whatever the shim does itself.
- [ ] Any generated `EXTRAS=` payload (decoded audio, splash) has a **script** that rebuilds it and
      verifies a checksum — never a lone copy in `packaging/stage/`, which every build wipes.
- [ ] One change per device test; results recorded in `plan/`.
- [ ] Ship check: unpack the final `.ipk` and read its `apkenv.env` — no debug/instrumentation vars;
      install it so the device runs what ships; clear `apkenv-snap-*.ppm` debris.

---

## Lessons from Temple Run 2 (Unity 3.5 + Mono), 2026-08-27

**Derive every native method's ARGUMENTS from the Java caller, not from its name.**
`UnityPlayer.nativeInit(II)` looks like `(width, height)` and is `(glesMode, splashMode)`. Passing
the screen size there made the engine build its fixed-function renderer (so every ES2-only shader
drew as magenta error material) and skip its splash screen - and cost a whole session of
disassembly to find and patch the branch that was faithfully reading the value we fed it.
The same class of bug: `nativeTouch`'s trailing int is the MotionEvent **source** (`0x1002`), not
padding. **Read the caller for each argument; a plausible signature is not a contract.**

**Prefer fixing the contract over patching the engine.** Once `nativeInit` got real arguments, the
binary patch that forced Unity's GLES2 device became unnecessary. A patch that works is evidence
you have found the mechanism - not that you have found the cause.

**Instrument the path the engine actually takes, and prove your probe works.**
- A seek probe cannot see sequential reads (we concluded "read once, never again" from 22 seeks;
  byte accounting showed a 64 KB buffer).
- Counting `pthread_create` calls through apkenv's own hook counts a subset; `/proc/self/task`
  showed the threads we had "proven" did not exist (with `comm` names like `FMOD stream thr` and
  `wchan`, which is far better evidence anyway).
- Always run a **positive control**: `MONO_VERBOSE_METHOD=<name>` prints nothing both when a
  managed method is never called AND when the probe is misconfigured. `=Awake` proves the
  mechanism before `=StartMainMenuMusic` is allowed to mean anything.
- `__builtin_return_address(0)` must be taken in the wrapper itself; inside a helper it reports
  the wrapper, and it silently "works" wherever the helper happens to be inlined.

**Check you are reading the real engine.** Temple Run 2's `lib/armeabi-v7a/libunity.so` is a 43 KB
**proxy**; the 6.8 MB engine sits inside `assets/libs/armeabi-v7a/`. A `strings | grep` on the proxy
returns nothing and reads exactly like a finding. Before concluding "the engine never mentions X",
check the file size against the job it supposedly does.

**Nothing covers the load unless you cover it.** On Android the Activity's window is what the player
stares at while a game loads; a shim has no Activity, so the panel just holds the last swapped
buffer — black. Do not go hunting for the engine's own splash path first: for Temple Run 2 neither
libunity nor the Java host references `splash.png` at all (`splash_mode` in settings.xml is only a
scaling policy), so a previous session spent a run forcing presents to reveal a draw that was never
issued. **The splash is the window's job, and the shim is the window** — draw it yourself, ride the
present path you already have, and retire it on the engine's first draw call (`apkenv_gl_draws`),
which needs no timer. Decode the image to raw RGB at package time, bottom row first for GL's origin.

**Seeing the screen beats reasoning about it.** `APKENV_GL_SNAPSHOT=<frame>` (glReadPixels -> PPM)
is the only way to see a GL app on webOS 3.0.5. Bind the WINDOW framebuffer before reading, or a
render-to-FBO port captures the offscreen target at the wrong width and you get a tiled, skewed
image that looks exactly like a broken renderer.

**Managed code is inspectable.** `tools/ildump.py` resolves the raw metadata tokens in Mono's
verbose dumps to `Class:Method` and finds a method's callers - no monodis needed. That is how
"who starts the music" was answered.

**Know when to stop.** If several device cycles in a row change nothing the user can see or hear,
that is the signal to report the state and pick a different attack - not to deploy again.

