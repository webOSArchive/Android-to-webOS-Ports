# PORTING-PLAYBOOK.md — how to bring the *next* Android NDK game to webOS

Distilled from the shipped ports (Where's My Water?, Plants vs. Zombies HD, Amazing Alex HD, Temple
Run 2, Aralon) and one that stalled (WMW2). `android-port-shim.md` is the architecture field guide; this is the **method** — what to
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
5. **Find `JNI_OnLoad` and call it.** `nm -D <engine>.so | grep JNI_OnLoad`. Dalvik runs it at
   `System.loadLibrary` time and it is where an engine `RegisterNatives` **its own callbacks** — the
   Java methods declared `native` in the smali. A module that skips it leaves every one of them
   unbound, and the failure is silent and remote from the cause: Fruit Ninja's worker thread called
   `NativeGameLib.native_threadEntry`, got nothing, and exited one log line later — and that thread
   was the one that creates the audio sink, so the whole game was mute with no error anywhere.
   Resolve it from the *game* lib by name (`LOOKUP_LIBM("lib<game>", "JNI_OnLoad")`); apks often
   ship a second `.so` with a `JNI_OnLoad` of its own.
6. **Split the contract by modifier.** A contract row declared `native` in the smali is an engine
   **callback**, not a host service: answer it by calling the engine's own registered function
   (`jnienv_find_native_method(class, name)`), never by inventing a return value.
   **A `native` Java method with NO caller anywhere in the dex is the tell for a JNI round-trip**:
   the engine calls its own Java static purely to re-enter native code (to attach a thread to the
   VM, or to pick up a Java-side object on the way). Both ports hit it — Fruit Ninja's
   `native_threadEntry`, Dead Space's `EAIO.Startup`/`rwfilesystem.Startup` — and in both the first
   reading was "dead code". `grep -rl <name>` finding only the declaration means **answer it**, not
   ignore it. Scope the name
   scan to the game's own Java packages — a modern apk bundles half a dozen ad/analytics SDKs whose
   `init`/`start`/`read`/`close` collide with unrelated strings and bury the real contract
   (189 useful rows vs 955; `apkenv/tools/fn-contract.sh` does this for a Mortar apk).
7. **Check JNI signatures per host generation.** Same method name ≠ same args: Airplay's
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

**A probe must cover every path the engine can take, or its negative result is a lie.**
`APKENV_TRACE_FILES` traced only `fopen`/`open`. Dead Space's engine **probes with `stat`/`opendir`
first** (it imports `stat`, `opendir`, `readdir`, `chdir`, `getcwd`), so when its content path was
wrong the trace said *"the engine opens no content file at all"* — true, and completely misleading:
it was looking, failing, and never reaching `open()`. That false negative sent a whole run into a
GL theory. `stat`/`opendir` are traced under the same flag now. Before believing "the engine never
asks for X", check which call it would ask *with*.

**apkenv's crash dump `on stack 0x...` lines are a stack SCAN, not a backtrace.** They list
everything on the stack that looks like a code address, including long-dead frames. Reading Adreno
entries there as "it is inside GL right now" is how a Dead Space crash got attributed to the
renderer when it was a missing asset. `pc` is real; `lr` may be garbage (it read `0x17c` once); the
scan is a hint list, nothing more.

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

**You can test touch without a finger.** `APKENV_MORTAR_AUTOTAP="x,y@frame;x1,y1>x2,y2@frame"`
(`modules/mortar.c`) presses, drags and releases through the module's own `input()` path, so it
exercises the real contract — normalized coordinates, Android action codes, `ACTION_POINTER_DOWN`
for a second finger — rather than a shortcut around it. Paired with `tools/grab.sh` it took Fruit
Ninja from "boots" to "scored 4 points in Classic mode, sliced a bomb" with nobody in the room, and
it makes "taps do nothing" reproducible instead of anecdotal. Copy the pattern into the next
module; keep it env-gated and out of every shipped `apkenv.env`.

**Input focus comes from the launcher, not from novacom.** A binary started with
`novacom run` renders fine but receives **no SDL input at all** — `ev_total=1` over 2400 polls
and, tellingly, **no `SDL_ACTIVEEVENT`**. Installed and launched from its icon, the same binary
gets a normal stream of `SDL_MOUSEBUTTONDOWN`/`UP`. Foreground vs detached makes no difference.
**Test touch from a packaged `.ipk`**, and do not spend time debugging "dead input" until you have.

**Ask the engine for the orientation; don't infer it from the screen.** Temple Run 2 *looks*
like a portrait game and is one on phones, but this build calls `setOrientation(0)` =
`SCREEN_ORIENTATION_LANDSCAPE` and its manifest declares none. Handling `setOrientation` as a
logged no-op is cheap and settles the question before anyone builds a rotation path.

**An asynchronous host operation must stay asynchronous.** Fruit Ninja's `HttpClient.HttpRequest`
runs on a Java `Thread` and publishes into public fields the engine polls (`IsFinished`, then
`ResponseCode`/`Result`/`ReturnedHeaders`). Answering it *inside the call* — IsFinished already true
when `HttpRequest` returned — made the engine parse a response for a request it had not finished
registering, and it dereferenced NULL in its own NetworkManager. Retiring it ~30 frames later fixed
the crash. Reproduce the host's **timing**, not just its values; and reproduce the **failure path it
actually has** (that one: `Result = new byte[0]`, `IsFinished = true`, `ResponseCode` left 0,
`ReturnedHeaders` left null — exactly what an offline device produces, which the game already has a
screen for). Also set the fields the Java worker sets and you did not think mattered: the engine
finds its own request context through `RequestPointer`.

**Dialogs / blocking host calls.** Text entry (`getInputString` → native `setInputText`), error
boxes (`showError`), anything modal: answer them synchronously from the module (check the engine
clears its result slot *before* the call — then an in-call answer is race-free). Never answer
empty strings where the game re-prompts.

**Audio (three shapes seen).**
- FMOD → `audio/fmod_pump.c` (AudioTrack-style pull pump; WMW).
- **Engine mixes, Java just plays** → `audio/audiotrack.c` directly. Halfbrick Mortar's
  `MortarAudioMixerOut.Create()` + `WriteData([B/[S)` is a bare `AudioTrack(44100, STEREO,
  PCM_16BIT, MODE_STREAM)`; read the ctor for the rate/format instead of guessing, hand the bytes
  to `apkenv_audiotrack_write` and let its blocking back-pressure pace the engine's audio thread.
  The easiest shape there is — *if* the engine's thread is alive (see `JNI_OnLoad` in §2). Prove it
  with a periodic meter, not a one-shot line: bytes/second against `rate*channels*2` says the pump
  tracks real time, and an underrun delta of 0 says the ring never ran dry.
- Marmalade `SoundPlayer.generateAudio(short[], nFrames)` → mixer **post-mix hook**
  (`apkenv_mixer_set_postmix`, SDL `Mix_SetPostMix`); `nFrames` is per-channel; widen mono→stereo.
- MediaPlayer music (`audioPlay(path)`) → SDL_mixer music. Device facts: the TouchPad's
  `libSDL_mixer` has vorbis **and** an ffmpeg MP3 decoder but **will not resample or remix music** —
  open the mixer 44100 Hz stereo and ship music at exactly that (`ffmpeg -ac 2 -ar 44100 -c:a
  libvorbis`). Paths arrive relative (Android cwd is `/`): try `/`+path and an `.ogg` twin.
  Make `load_music` return NULL on failure (upstream returned an empty wrapper = silent success).
- **When the engine's own stream is stuck, carry the music over the path that already works.**
  Unity's native FMOD stream in Temple Run 2 primes one 64 KB buffer and never consumes it, and
  inline-hooking libunity's FMOD internals to force it **crashed the device**. Everything else had
  been ruled out (our pump, file IO, volume, game logic, `fmodInitJni`, missing threads). Shipped
  instead: the game's own music track, decoded at package time, mixed into the *known-good*
  AudioTrack pump after `fmodProcess()` and before the ring write — `APKENV_FMOD_MUSIC_PCM`, so no
  other game changes. What makes that honest rather than a fake: the game's **own** asset; **its
  format exactly** (24000 Hz stereo S16 — FMOD's *measured* rate, since the pump does no
  resampling); wired to the real control (`APKENV_FMOD_MUSIC_PREF` → the in-game slider); gain
  **ramped across the chunk** (a step at a buffer boundary clicks); and the limits written down (it
  loops, and does not follow the per-scene music source). *"The mechanism is unfixable" and "the
  outcome is undeliverable" are different claims — check the second before spending another
  session on the first.*
- **Assets are not always one file per file.** That music lived in
  `sharedassets0.assets.resS` as **two concatenated MP3s with no container** (Unity keeps the
  offsets elsewhere); decoding it whole silently yields both tracks back to back. Split first.
- **Effects play but music is silent? Check thread creation before the audio path.** Aralon (Unity 4)
  had exactly that, and the cause was not audio at all: FMOD asks for an **8 KB** stack for its "FMOD
  file thread" (sized for bionic), glibc rejects stacks under 16 KB with EINVAL, and FMOD maps *any*
  `pthread_attr_*`/`pthread_create` failure to `FMOD_ERR_INTERNAL` (33). Every sound that needs async
  file I/O (MP3 music, streams) then fails `createSound` — and Unity 4 stores that failure silently
  (AudioManager+156/+180) and keeps a "ready" clip of length 0, so `Play()` does nothing and nothing is
  logged. Raising only to 16 KB was not enough (the process died at thread start: glibc also keeps the
  thread descriptor on the stack, and apkenv's trampoline logs via unbuffered stderr, which puts an
  8 KB buffer on the stack). Fix: `APKENV_PTHREAD_STACK_CLAMP=1` → 128 KB floor
  (`compat/pthread_wrappers.c`). Tell-tale in a thread sample: a thread the real device has (`FMOD
  file thread`) that yours never gets. Only 3 `pthread_create` calls in the log while more threads
  exist is the same clue: the failing thread dies *before* `pthread_create`.
- **Watching a Unity engine's audio without patching it:** libunity calls FMOD directly (no PLT), so
  FMOD itself can't be interposed, but every `UnityEngine.AudioSource/AudioClip` internal call is
  registered through `mono_add_internal_call` — which, with `APKENV_HOST_MONO`, is ours.
  `APKENV_UNITY_ICALL_TRACE=1` (`compat/icall_trace.c`) puts logging trampolines in front of them and
  reports each `Play()` with the clip's length, `isReadyToPlay` and `isPlaying` afterwards. Always run
  the positive control (a sound effect's `PlayOneShot` length) before trusting a 0.
- **A real Android device is the fastest oracle.** Install the original apk (+ OBB at
  `/sdcard/Android/obb/<pkg>/`) with `adb`, play the same stretch, and diff: `logcat` (Unity's own
  messages), `/proc/<pid>/task/*/{comm,stat}` for thread names and CPU (readable without root), and the
  AudioTrack setup lines. It turned "should there be music here?" into a fact, and showed the one
  missing thread. It also settles "does this feel slow?", which nobody can answer from memory —
  a same-era tablet side by side did it for Fruit Ninja in minutes. Drive it with
  `adb shell input swipe x1 y1 x2 y2 <ms>` (the `APKENV_MORTAR_AUTOTAP` idea, for free), and for a
  number rather than an impression use `dumpsys SurfaceFlinger --latency "<layer>"` — the layer name
  comes from `--list`, and column 2 of each row is the vsync the frame was actually presented at, so
  the deltas are frame times. `dumpsys gfxinfo` will NOT work for these games: it measures the UI
  toolkit, and an NDK game draws to its own GLSurfaceView.

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

**Does the game adapt to the surface aspect? Measure it, do not argue about it.** A wrong-aspect
game usually splits: the **3D adapts and the 2D UI does not**. Dead Space on 4:3 overlapped its menu
buttons while playing fine. Log the projection (`APKENV_GL_DEBUG` → `[GLPROJ]` in
`compat/gles_wrappers.c`, which hooks `glLoadMatrixf` when `matrix_mode == GL_PROJECTION`, because
plenty of engines build their own matrices and never call `glFrustumf`) and run the same scene at
two aspects:
- **perspective** `m[0] = 1/(aspect·tan(fovy/2))`, `m[5] = 1/tan(fovy/2)`. Constant `m[0]` with
  changing `m[5]` = horizontal FOV held, vertical follows the surface, so **a taller surface really
  sees more and letterboxing costs field of view**. (Dead Space: hFOV 53.4° both ways, vFOV 41.3°
  at 4:3 vs 34.4° at 16:10.)
- **ortho** gives the UI's design space directly — Dead Space's is a fixed **320 units tall**,
  `320·aspect` wide, so elements placed for the wider design collide at 4:3. That is the whole bug,
  and it says exactly how much width the UI needs.
Then letterbox to the *narrowest* aspect the UI tolerates rather than the game's nominal one
(3:2 not 16:10, here), so the 3D keeps what it can. And note the argument the other way: a taller
surface shows **more than the game was framed for**, which is not automatically better — though a
game that letterboxes its own cutscenes has already answered that.

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
  **(4)** `novacom: unexpected EOF from server` is how novacom reports a remote **non-zero exit**
  (`pidof` finding nothing, `ls` of a missing file), not a dead link — `novacom -l` tells the two
  apart.
  **(5)** `kill -9` (which every iteration script uses, because `killall` does not reliably take)
  **leaves the app's PDK jail bind-mounts behind** — webOS only tears them down on a clean exit.
  Note what the count means before reading anything into it: **one jailed app is ~23 bind mounts**
  (`/media/internal`, `/usr/lib`, … all bind-mounted into `/var/palm/jail/<appid>/`), so 23 lines in
  `/proc/mounts` is a single jail, not 23 leaks — check `sed 's|.*jail/||;s|/.*||' | sort -u` for the
  app count. What was observed once: a jail left behind by an app that had since been *uninstalled*,
  alongside `/media/internal` unmounted entirely (`store-media` present in `/dev/mapper`, absent from
  `/proc/mounts`). Whether the stale jail caused that or merely accompanied it is **not
  established**. That
  surfaces as `palm-install` failing with `novacom error ... file open failed` on
  `/media/internal/.developer`, which reads exactly like a corrupt package. Before blaming the
  `.ipk`: `grep -c palm/jail /proc/mounts` and check `df` for `/media/internal`. A reboot clears
  both. Symptom to watch for earlier: `ls` of the app dir returning
  `Resource temporarily unavailable` (cryptofs is backed by the media partition).
- **Binary-only iterations on an installed app:** `novacom put` the new `apkenv` into
  `/media/cryptofs/apps/usr/palm/applications/<appid>/` and `palm-launch`; rebuild the package only
  when the env or assets change. A put over a binary a live process still holds fails (`file open
  failed`), and a script that carries on launches the **old binary with the new env** — a wasted run
  (Aralon A8). Kill until `pidof apkenv` is empty, push, and **refuse to launch unless the md5 on the
  device matches** the local build.
- **When the engine cannot read assets out of the apk, do not ship the apk twice.** Dead Space's
  BLAST engine opens content with plain `fopen`/`mmap` and has no zip reader and no
  `AAssetManager`, so its 319 MB has to be real files. Bundling the whole apk *and* the extracted
  tree is a ~615 MB package, over half dead weight. Instead **strip the apk** to
  `lib/ + AndroidManifest.xml + resources.arsc + res/` (2.6 MB — apkenv only needs it to load the
  engine, name the app and yield an icon) and ship the content tree with **`EXTRAS=`, not `DATA=`**:
  `DATA=` seeds a first-run copy into `/media/internal`, `EXTRAS=` lands it in the app dir to be
  read in place. 174 MB, nothing duplicated. Give the staging step a script that verifies the staged
  tree matches the apk's **file count and byte total** — a short content tree is a game that boots
  and then cannot find a level.
- **The path the engine composes may not contain the apk's package name.** Dead Space builds
  `<GetExternalStorageDirectory()>/Android/data/**com.ea.deadspace**/files/published/...` while the
  apk is `com.eamobile.deadspace_full_azn`. Read the path off a `stat` trace; do not derive it.
- **Big packages:** a 291 MB `.ipk` (Aralon, OBB inside) spends ~5 min in the USB copy and ~3 more
  while the device unpacks it. An install timeout shorter than that launches a half-installed app
  (`tools/tr2-run.sh` takes `INSTALL_TIMEOUT`, default 1800 s).
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
- [ ] **`JNI_OnLoad`**: does the engine export one? Call it before anything else — it is where the
      engine binds its own callbacks, and skipping it fails silently and far from the cause.
- [ ] Host contract table: engine→Java names vs module handlers; blocking-capable gaps ranked;
      rows declared `native` are the engine's callbacks, answered from the RegisterNatives table.
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
- [ ] Synthetic input (`APKENV_MORTAR_AUTOTAP`-style) + `tools/grab.sh` so the port can be driven
      through menus and gameplay without waiting on a person.
- [ ] One change per device test; results recorded in `plan/`.
- [ ] **Unity?** Which host generation (`nativeSetInputCanceled` in the native table = Unity 4's
      order); add `plan/<game>-mono-imports.txt` and regenerate `compat/mono_symbols.h`; IL-scan the
      managed DLLs for the `AndroidJavaObject`/`AndroidJavaClass` calls the reflection bridge must
      answer.
- [ ] **Expansion file?** `main.<ver>.<pkg>.obb` → `EXTRAS=`, `APKENV_UNITY_OBB`, `APKENV_UNITY_PACKAGE`;
      budget install time for the package size.
- [ ] **Effects but no music** → thread creation first (`APKENV_PTHREAD_STACK_CLAMP=1`), then the
      audio path. "Is X supposed to be here?" → play the original apk on a real Android device.
- [ ] Visual claims come from `tools/grab.sh`, never the on-device screenshot.
- [ ] Ship check: unpack the final `.ipk` and **look at `icon.png`** — and note that
      `build-ipk.sh`'s explicit `ICON=` branch **copies without resizing** (the auto-detect branch
      resizes to 64x64), so pass an already-downscaled file, not the source art — `build-ipk.sh` used to pick
      it non-deterministically (`find | head -1`), so a rebuild could silently swap which artwork
      shipped; Fruit Ninja carried the paid icon while its manifest declared the free one. It now
      asks `aapt` for the manifest's icon and prints what it used — read that line. Then read its
      `apkenv.env` — no debug/instrumentation vars;
      install it so the device runs what ships; clear `apkenv-snap-*.ppm`/`apkenv-grab.ppm` debris.
- [ ] **Fresh-install check before calling it released:** uninstall (`palm-install -r`), move the save
      dir aside, install, verify the installed binary's md5 and env on the device, launch on the fresh
      profile, and confirm the fix's own log line *and* what the player sees and hears.

---

## Lessons from Temple Run 2 (Unity 3.5 + Mono), 2026-08-27

**Derive every native method's ARGUMENTS from the Java caller, not from its name.**
`UnityPlayer.nativeInit(II)` looks like `(width, height)` and is `(glesMode, splashMode)`. Passing
the screen size there made the engine build its fixed-function renderer (so every ES2-only shader
drew as magenta error material) and skip its splash screen - and cost a whole session of
disassembly to find and patch the branch that was faithfully reading the value we fed it.
The same class of bug: `nativeTouch`'s trailing int is the MotionEvent **source** (`0x1002`), not
padding. **Read the caller for each argument; a plausible signature is not a contract.**

*It recurred immediately.* Dead Space's `AndroidEAAudioCore.Init(AudioTrack,III)` reads like
`(track, rate, channels, bufferBytes)` and is `(track, framesPerBuffer, channels, **sampleRate**)` —
the rate is LAST. Passing the plausible order told the engine its output rate was 8192 Hz while the
device drained at 44100: audible as chirpy, stuttering sound, with 2.9 MB of ring underrun in 20 s.
One transposition, both symptoms, and neither of them looks like "wrong argument" from the outside.
The caller computes `bufsize / (sizeofShort * channels)` right in front of the call — thirty seconds
of reading that was worth more than any amount of listening to the result.

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
image that looks exactly like a broken renderer. For a running game, on demand:
`tools/grab.sh` (see the Aralon lessons below).

**Managed code is inspectable.** `tools/ildump.py` resolves the raw metadata tokens in Mono's
verbose dumps to `Class:Method` and finds a method's callers - no monodis needed. That is how
"who starts the music" was answered.

**Know when to stop.** If several device cycles in a row change nothing the user can see or hear,
that is the signal to report the state and pick a different attack - not to deploy again.

---

## Lessons from Aralon (Unity 4.0.1 + Mono), 2026-09-14

Full trail: `plan/ARALON.md`. Everything below is gated (Unity 4 host detection or an opt-in env
var), so Temple Run 2's path is unchanged.

**Same engine family is not the same host contract.** Read the new generation's `UnityPlayer.smali`;
don't reuse the last port's call order. Unity 4's host calls `nativeResize` and one `nativeRender`
*before* `unityAndroidInit` (`onSurfaceChanged` and the first `onDrawFrame` both run before init), and
that pre-init resize is the only thing that builds the loading-screen blitter. Skipping it gave a
SIGSEGV inside `unityAndroidPrepareGameLoop` (a NULL global at `libunity+0x78883c`). Unity 3.5 resized
*after* prepare. `modules/unity.c` detects the generation by `nativeSetInputCanceled` in the native
table and follows that host's order. *A native's behaviour can depend on engine state: "we call it"
is not "we call it when the host does."* Other Unity 4 deltas: `unityAndroidInit` returns `Z`, and
each OBB is registered with a second `nativeFile(path)`.

**The Java contract has a hidden half: reflection.** §2's grep finds the Java methods the *engine*
names. Unity 4's C# scripts reach Android through `AndroidJavaObject`, i.e. Unity's
`ReflectionHelper.getMethodID/getFieldID/getConstructorID` → `FromReflectedMethod/Field` →
`Call*MethodA`/`NewObjectA`/`Get*Field`. The targets (`getWindowManager`, `getMetrics`,
`widthPixels`) are strings in the game's managed DLLs, not in the smali host or libunity. Under a fake
JNI every link returns 0/NULL silently. Aralon read a 0×0 `DisplayMetrics`, sized its whole GUI from
it, and showed an **invisible menu that ignored taps**, while the 3D scene rendered perfectly and taps
did reach `nativeTouch`. Tells in the game's own log: `JNI: Unable to find method id for
'getMetrics'`, `Init'd AndroidJavaObject with null ptr!`. Find the calls up front by IL-scanning the
DLLs (`tools/ildump.py`) for `AndroidJavaObject`/`AndroidJavaClass` users. The bridge in
`modules/unity.c` answers by name; extend it for the next game's calls. `NewGlobalRef` must return
the object it was given: NULL for a real object breaks every AndroidJavaObject a script keeps.

**Exported is not bridged.** The host Mono exported all 119 symbols Aralon's libunity imports, and the
first run still died with `cannot locate 'mono_string_new_len'`: the bridge list was TR2's. Each Unity
port adds `plan/<game>-mono-imports.txt` (`comm -12 <(libunity UND) <(host libmono DEF)`).
`tools/gen-mono-hooklist.sh` with no arguments emits the union. Rebuild afterwards, because the build
does not track headers (§5).

**Expansion files (OBB).** Games over Google Play's apk size limit keep their data in
`main.<versionCode>.<package>.obb`, a zip. Ship it via `EXTRAS=` (lands in `android/extras/`), point
`APKENV_UNITY_OBB` at it, and set `APKENV_UNITY_PACKAGE`. It is most of the install: 273 of Aralon's
283 MB on the device. Test the install time before trusting a script's timeout.

**A feature can fail silently into a field.** Unity stored FMOD's `createSound` failure in the
AudioManager and kept a "ready" zero-length clip, so music was simply absent, with nothing in the
log. When something the player expects is missing and nothing complains, look for where the engine
*stores* errors (§4 Audio has the whole chain, the stack-size cause, and the icall tracer that
exposed it).

**Screenshots: read the GL frame, and trust the eye on the panel.** The on-device screenshot tool
does not capture the GL layer faithfully. Aralon's ground came out solid black in a capture, the user
saw it merely darker, and a "terrain is not drawn" theory built on the capture cost three runs
(upload check, opaque present) before the user corrected it. Take every screenshot with
`tools/grab.sh` (a GL readback of the running game, all layers, 1024×768 PNG). For a brightness or
colour difference against another device, the user's eyes on both panels are the measurement: a
Mali tablet's panel is not the TouchPad's.

**Refuted for Aralon's silent music. Don't re-chase these in the next game until the clamp is on:**
volume 0, the music option off, music never requested, the bridged libm, OpenSL decoding, the idle
`FMOD stream thr`, lost `asyncProcessor` wakeups, and the CPU count (`/sys/devices/system/cpu/present`
is missing on webOS; `APKENV_SYS_CPU=1` answers it faithfully but was not the cause). What cracked it
was a **thread the Android reference had and we didn't**.

**Diagnostics added (all opt-in, all off in the release):**
- `APKENV_UNITY_ICALL_TRACE`: audio icalls plus FMOD's stored error.
- `APKENV_GL_UPLOADCHECK`: `glGetError` around every texture/buffer upload in *both* wrapper tables,
  with errors handed back on the engine's next `glGetError` and a format census.
- `APKENV_TRACE_SEEK_RANGE`: up to 16 ranges, with per-range byte totals.
- `APKENV_SYS_CPU`.
- `APKENV_OPAQUE_PRESENT`: alpha forced to 1 before the swap, for a game whose framebuffer alpha
  lets the compositor show through.
- `tools/grab.sh`.

---

## Lessons from Fruit Ninja (Halfbrick Mortar), 2026-09-15

Full trail: `plan/FRUITNINJA.md`. Ported in one session, ten device runs, no crashes in the final
build. The general points are folded into §2, §3 and §4 above; the two worth stating plainly:

**The static pass can be confidently wrong, and the tracer is what catches it.** The contract
derivation found exactly one reference to `native_threadEntry` — its own `JNINativeMethod` entry —
and concluded "dead code from an older build". It is in fact how the engine attaches its audio
thread to the VM. The always-on unhandled-call tracer printed it on the first device run, one line
before the thread's `<<< end`. **Ship the tracer before the first run even when the static pass
looks complete** — especially then, because that is when you will not go looking.

**One root cause can be behind several unrelated-looking symptoms.** No sound, a worker thread that
exits instantly, and dialogs that could never have been answered were all the same missing
`JNI_OnLoad`. Before theorising separately about an audio path and a threading bug, check whether
one unbound mechanism explains both.

---

## Lessons from Dead Space (EA Mobile BLAST), 2026-09-15

Full trail: `plan/DEAD-SPACE.md`. Triage to a released, playable 174 MB `.ipk` in one evening. The
specifics are folded into §2–§5 above; three things are worth stating on their own because each one
cost a device run or sent the work sideways.

**The most expensive thing was a confident negative from an incomplete probe.** "The engine opens no
content file at all" was *true* and led straight to a GL theory with no basis, because the tracer
watched `fopen`/`open` and the engine probes with `stat`/`opendir`. A negative result is the weakest
evidence an instrument can give; verify the instrument covers the path before you believe it.

**Two ports, two transposed-argument bugs, both invisible from the outside.** Temple Run 2's
`nativeInit(II)` was `(glesMode, splashMode)`; Dead Space's `AndroidEAAudioCore.Init(AudioTrack,III)`
is `(track, framesPerBuffer, channels, sampleRate)` — rate last. The second one produced "chirpy,
stuttering audio" *and* 2.9 MB of ring underrun, which read as two separate problems. Neither looks
like "wrong argument". **Read the caller for every argument, every time** — the Dead Space caller
computes `bufsize / (sizeofShort * channels)` on the line above the call.

**The operator's observations are data, and the imprecise ones are often the most valuable.**
"Chirpy" (not just "stuttering") is what distinguished a rate bug from starvation. "In-game looked
fine without letterboxing" is what prompted measuring the projection and finding that the 3D adapts
while only the UI is hard-coded — which turned a blunt 16:10 letterbox into a 3:2 one that keeps
most of the field of view. Neither would have come from the logs.
