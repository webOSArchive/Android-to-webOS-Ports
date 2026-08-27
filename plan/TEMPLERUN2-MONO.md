# Temple Run 2 — Stage M: native (glibc) Mono runtime for apkenv

Date: 2026-08-27. Author: Fable 5 (planning). Executor: Opus, autonomously.
Parent trail: `plan/TEMPLERUN2.md` (why we are here). Method: `PORTING-PLAYBOOK.md`.

## 0. The one-paragraph brief

`libunity.so` (bionic, from the apk) dies inside the bionic `libmono.so` during `mono_jit_init_version`
because Mono is the one component that does everything glibc-hostile at once (signals, GC stack
scanning, thread attach, JIT pages) on top of apkenv's bionic↔glibc bridge. Replace it: build the
**exact same Mono** (Unity fork, `mono 2.6.5`, branch `unity3.5`) **natively against the device
glibc** with the PalmPDK toolchain, load it on the *host* side with `dlopen`, and export its
`mono_*` symbols into apkenv's hook table so `libunity`'s 118 imports bind to the native runtime.
Blacklist the apk's `libmono.so`. The libunity↔libmono boundary is a plain C ABI (softfp EABI on
both sides), so this is a bridge, not a rewrite.

## 1. Facts established (do not re-derive)

| Fact | Value | How verified |
|---|---|---|
| Mono version in apk `libmono.so` | **2.6.5** (`strings`) | 2026-08-27 |
| Unity version | 3.5.7f6 | strings in libunity |
| Upstream | `github.com/Unity-Technologies/mono` — branch **`unity3.5`** (`64c3378a`), also `unity3.5-staging` (`10e1bc7e`), tag `unity3.5.0` (`824546eb`); all `AM_INIT_AUTOMAKE(mono,2.6.5)`, `MONO_CORLIB_VERSION 82` | GitHub raw + `git ls-remote` |
| libunity → libmono imports | **118** `mono_*` symbols; all exist in the apk's libmono; list in §A | `nm -D` diff |
| Unity-private exports libunity needs | `mono_file_map_override`, `mono_set_find_plugin_callback`, `mono_unity_set_embeddinghostname`, `mono_unity_socket_security_enabled_set`, `mono_verifier_set_mode`, `mono_set_signal_chaining`, `mono_security_set_core_clr_platform_callback` | `mono_set_find_plugin_callback` confirmed in `unity3.5:mono/metadata/loader.c:1221`; `mono_unity_*` live in `mono/metadata/socket-io.c`; the rest must be grepped after clone (§3.1) |
| **Fast-TLS theory from yesterday: REFUTED** | 0 × `mrc p15,0,rX,c13,c0,3`, 0 × kuser `0xffff0fe0` literals in libmono; `mono_pthread_key_for_tls` is a plain table lookup with no callers | byte scan + objdump |
| Toolchain | PalmPDK `arm-none-linux-gnueabi-gcc 4.3.3`, sysroot glibc **2.5** headers, links to device glibc 2.4 symbols, softfp, ARMv7/NEON | `/opt/PalmPDK/arm-gcc` |
| Host autotools | autoconf 2.71, automake 1.16.5, libtool, bison, gettext present | `which` |
| Managed P/Invoke modules | `Assembly-CSharp-firstpass.dll` P/Invokes **`__Internal`** (Prime31); no other native module names | `strings` on DLLs |
| Hook precedence | `linker.c:1368` consults `apkenv_get_hooked_symbol()` **before** library lookup for every relocation → a hook shadows the apk's own libmono. `HOOKS_MAX=1024`, ~360 used | read |
| NEEDED resolution | `linker.c:2060`: a NEEDED name that matches `builtin_libs[]` (`hooks.c:96`) needs no file; `dlopen`/`dlsym` of a builtin name route through `apkenv_get_hooked_symbol_dlfcn` | read |
| Blacklist | `apkenv.c:987 libblacklist[]` skips loading an apk lib by basename | read |
| Module | `modules/unity.c`; `unity_try_init` looks up `JNI_OnLoad` in `libmono` (optional) and `libunity` (required) | read |
| Corlib | `assets/bin/Data/Managed/mscorlib.dll` (1.36 MB, 2013-05-29) — is Unity's 2.6.5 corlib; runtime asserts corlib version == 82 at init | appdomain.c |

The TLS refutation removes one *motivation* but not the decision: the remaining case (Mono is the
largest libc-hostile surface; every mismatch below the libc line is a silent-corruption hunt, and a
glibc-native Mono deletes the entire class) stands. Do not reopen it; log any new evidence in §6.

## 2. Architecture of the bridge

```
apkenv (glibc, host)                         bionic world (apkenv linker)
┌──────────────────────────────┐             ┌──────────────────────────┐
│ dlopen("libmono-webos.so")   │             │ libunity.so              │
│   → hook table: 118+ mono_*  │◄── PLT ─────│  imports mono_*          │
│ builtin_libs += "libmono.so" │             │  NEEDED libmono.so  (ok) │
│ blacklist apk libmono.so     │             │  (apk libmono NOT loaded)│
│                              │             │                          │
│ libmono-webos.so (glibc)     │── fn ptrs ─►│ internal calls, plugin   │
│   JIT'd C# code              │  (softfp)   │ finder, file-map override│
│   GC, threads, signals =     │             │                          │
│   plain glibc                │             │                          │
└──────────────────────────────┘             └──────────────────────────┘
```

Rules that make this sound:
- Both sides are ARM EABI **softfp**; struct layouts crossing the boundary are Mono's public ones
  (`MonoObject`, `MonoString`, `MonoArray`, `MonoDomain*` opaque) and identical because it is the
  *same* source tree. Verify `MonoObject`/`MonoString`/`MonoArray` in `mono/metadata/object.h` were
  not changed by Unity vs. the apk (they weren't in 2.6; note it once).
- `libunity` passes callbacks INTO Mono (internal calls via `mono_add_internal_call`, the plugin
  finder, `mono_file_map_override`); Mono calls them with plain `blx`. They execute bionic code that
  itself calls hooked libc — already how every port works.
- Threads: libunity's `pthread_create` is already hooked to glibc, so every thread Mono attaches or
  scans is a real glibc thread. Boehm GC's stack-bottom discovery (`pthread_getattr_np`) is now
  glibc-on-glibc — the bionic `pthread_attr_t` size hazard listed in `TEMPLERUN2.md` disappears.
- Signals: native Mono installs its own SIGSEGV/SIGFPE/SIGILL handlers with chaining
  (`mono_set_signal_chaining(1)` is called by libunity); apkenv's crash handler (`debug/debug.c:223`)
  is installed *before* and will be chained to. Managed null-refs are SIGSEGVs Mono expects to
  handle — the crash handler must NOT be first for those. Check ordering in §3.4.
- P/Invoke `__Internal` resolves via `dlopen(NULL)` in native Mono = the apkenv executable. Prime31
  symbols don't exist there → managed `EntryPointNotFoundException` when Prime31 code runs, which is
  *expected and acceptable* (stub later via `mono_add_internal_call`-style override or a host-side
  `__Internal` provider). Not a Stage-M blocker.

## 3. Stages (each has a checkpoint; do not proceed on an unmet checkpoint)

### 3.1 Source pin + private-export audit (offline, ~30 min)
```
cd <scratchpad>; git clone --branch unity3.5 --depth 50 https://github.com/Unity-Technologies/mono.git mono-unity3.5
cd mono-unity3.5
for s in $(cat <workspace>/plan/tr2-mono-imports.txt); do grep -rlq "^$s\b\|[^a-z_]$s *(" mono/ || echo "MISSING $s"; done
```
- Also confirm by grep: `mono_file_map_override` (expect `mono/utils/mono-mmap*.c` or `mono/metadata/`),
  `mono_verifier_set_mode` (`mono/metadata/verify.c`), `mono_unity_*` (`socket-io.c`, and
  `mono_unity_write_to_unity_log` wherever it is).
- Compare `mono/metadata/object.h` `MonoString`/`MonoArray`/`MonoObject` to Mono 2.6.5 upstream (no diff expected).
- If any import is missing on `unity3.5`, try `unity3.5-staging`, then the `Mono2.6.x-Unity3.4` line;
  pick the branch with zero MISSING. Record the chosen commit hash in §6.

**Checkpoint A:** zero MISSING symbols on the chosen commit; commit hash recorded.

### 3.2 Cross-build the runtime only (~2–4 h of configure iteration; this is the bulk of the day)
Use the PalmPDK gcc **directly** (no two-toolchain trick — Mono has no GL headers). Build script:
`apkenv/tools/build-mono-webos.sh`, checked in, reproducible from a clean clone.

```
export PDK=/opt/PalmPDK/arm-gcc
export PATH=$PDK/bin:$PATH
export CC=arm-none-linux-gnueabi-gcc CXX=arm-none-linux-gnueabi-g++ AR=arm-none-linux-gnueabi-ar RANLIB=arm-none-linux-gnueabi-ranlib STRIP=arm-none-linux-gnueabi-strip
export CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=softfp -fsigned-char -O2 -g -DPLATFORM_WEBOS"
export CPPFLAGS="-I$PDK/sysroot/usr/include"  LDFLAGS="-L$PDK/sysroot/usr/lib -Wl,-rpath-link,$PDK/sysroot/lib"
./autogen.sh --host=arm-none-linux-gnueabi --build=x86_64-linux-gnu --prefix=/opt/mono-webos \
  --disable-mcs-build --with-glib=embedded --with-gc=included --with-tls=pthread \
  --with-sigaltstack=no --disable-shared-handles --with-static_mono=no \
  --disable-nls --with-libgdiplus=no --with-x=no --with-moonlight=no --with-oprofile=no \
  --enable-minimal=aot,profiler,com,simd \
  --with-large-heap=no --disable-parallel-mark
# Keep `debug` and `logging` OUT of --enable-minimal: we need Mono's own traces (MONO_LOG_LEVEL).
make -C eglib && make -C mono/utils && make -C mono/io-layer && make -C mono/metadata && make -C mono/mini libmono.la
```
Known cross-compile landmines for Mono 2.6 (pre-arm them as `configure` cache vars in the script rather
than discovering them one at a time):
- `mono_cv_uscore=no`, `ac_cv_func_mmap_fixed_mapped=yes`, `ac_cv_func_posix_getpwuid_r=yes`,
  `ac_cv_func_posix_getgrgid_r=yes` (or whatever the `--host` test can't run — configure lists them
  as "cannot run test program while cross compiling").
- eglib: `--with-glib=embedded` avoids needing a cross glib. Check `eglib/configure.ac` cache vars too.
- libgc (Boehm, `--with-gc=included`): `libgc/configure` needs `--host` forwarded; ARM stack-direction
  and `GC_LINUX_THREADS`/`_REENTRANT` come from `libgc/include/private/gcconfig.h` — ARM Linux is
  supported. Do **not** pass `--enable-parallel-mark` on this glibc.
- `--with-tls=pthread`: explicit. `__thread` needs the host compile test and buys nothing on ARM 2.6
  (no `MONO_ARCH_HAVE_TLS_GET` for ARM in this branch — verify in `mini-arm.h`; if it *is* defined,
  keep pthread anyway).
- `mono_arch_flush_icache` (`mini-arm.c:690`): `__GNUC_PREREQ(4,1)` with gcc 4.3.3 selects
  `__clear_cache` — correct for glibc; the `PLATFORM_ANDROID` raw-svc path must NOT be hit.
- `mono/utils/mono-sigcontext.h` ARM: assumes a glibc `ucontext` layout — good, we *are* glibc now.
- gcc 4.3.3 quirks: no `-Wno-unknown-warning`; strip any `-Werror`. C99 issues will show as
  `error: ... before 'for'` — fix locally, keep patches in `apkenv/tools/mono-webos.patch`.
- Link: `libmono.so` must have no `GLIBC_2.6+` versioned symbols. Verify:
  `readelf -V --dyn-syms libmono.so | grep -oE 'GLIBC_[0-9.]+' | sort -u` → max **GLIBC_2.4**.
  Common offenders: `__isoc99_*` (avoid `-std=c99`/`_ISOC99_SOURCE`), `epoll_create1`, `pipe2`,
  `eventfd`, `sched_getcpu`, `__sched_cpucount`. If configure detects one, force its `ac_cv_func_X=no`.
- `libmono.so` NEEDED must be only `libpthread.so.0 libm.so.6 libdl.so.2 librt.so.1 libc.so.6 [libgcc_s.so.1]`.

Output: `apkenv/libs/webos/libmono-webos.so` (unstripped copy kept in `build/webos/` for addr2line).

**Checkpoint B:** `file` says ARM EABI5 shared object; max symbol version GLIBC_2.4; `nm -D --defined-only`
contains **all 118** names from §A (script `apkenv/tools/check-mono-exports.sh` diffs them, exit 0).

### 3.3 Sanity on device without libunity (~30 min)
Tiny host program `apkenv/tools/monotest.c` (linked with PalmPDK gcc, like `apkenv`): 
`mono_set_dirs`, `mono_set_assemblies_path(<extracted assets/bin/Data/Managed>)`, `mono_jit_init_version("Unity Root Domain","v2.0.50727")`,
`mono_domain_assembly_open(mscorlib)`, `mono_runtime_invoke` of `System.Environment.get_Version`
→ print the version string; then `mono_jit_cleanup`. Push the apk's `Managed/*.dll` to
`/media/internal/tr2-managed/` (extract with `unzip` from the pristine apk; never modify the apk).

Run under the same jail conditions as the game later (`/var/apkenv2/` harness, as root first, then
as the jailed PDK uid if the harness supports it). Expected failure modes and their meaning:
- `expected corlib version 82, found N` → wrong corlib/runtime pairing; the branch pin is wrong (§3.1).
- SIGSEGV in `GC_init`/`GC_get_maps` → Boehm on this kernel/glibc; try `GC_DONT_GC=1`-style env to
  bisect (`MONO_GC_PARAMS`, `GC_MARKERS=1`), and check `libgc` was built with `-DGC_LINUX_THREADS -D_REENTRANT`.
- SIGILL → NEON/VFP mismatch; check `-mfloat-abi=softfp` reached the JIT's `mini-arm.c` (`MONO_ARCH_SOFT_FLOAT`? — 2.6 uses `-mfloat-abi=softfp` hardware VFP via `ARM_FPU_VFP`; set `--with-fpu=VFP` if configure has it, else `-DARM_FPU_VFP=1`).

**Checkpoint C:** `monotest` prints a `System.Version` on the TouchPad and exits 0. This proves
native Mono + Unity corlib + JIT + GC on webOS, independent of apkenv. Save the log as
`plan/logs/tr2-monotest-1.log` (logs dir is fine to commit; keep it small).

### 3.4 Host-lib bridge in apkenv (~2 h)
New file `apkenv/compat/hostlib.c` (+`.h`), general-purpose, not Mono-specific:
```c
/* Load a glibc .so on the host and publish a symbol list into the hook table. */
int apkenv_hostlib_bridge(const char *path, const char *const *symbols, size_t n, const char *builtin_name);
```
- `dlopen(path, RTLD_NOW|RTLD_GLOBAL)`; for each name `dlsym`; abort (loud) on any missing symbol
  *before* the game starts — a missing name later would resolve to the bionic lib or crash.
- `register_hooks()` with the resulting `struct _hook[]` (bump `HOOKS_MAX` to 2048).
- Add `"libmono.so"` to `builtin_libs[]` (new `BUILTIN_LIB_MONO`) so libunity's `NEEDED libmono.so`
  is satisfied without a file, and `dlsym` on that builtin handle (`hooks.c:145`) falls through to
  the global hook table (it already does for non-GL builtins).
- Add `"libmono.so"` to `libblacklist[]` in `apkenv.c` — but **only when the bridge is active**
  (`APKENV_HOST_MONO=/path` env var or module request), so WMW/PvZ/Alex are untouched.
- Symbol list = `plan/tr2-mono-imports.txt` (§A) compiled into `modules/unity.c` as a static array
  (generate with a script, keep the script: `apkenv/tools/gen-mono-hooklist.sh <libunity.so>`).
- `modules/unity.c`: when the bridge is active, skip `LOOKUP_LIBM("libmono","JNI_OnLoad")` and the
  call to it (the bionic libmono's `JNI_OnLoad` only stashed the JavaVM for Android socket/log glue).
  Grep the apk libmono's `JNI_OnLoad` disassembly briefly to confirm it does nothing else.
- Crash-handler ordering: Mono installs its handlers in `mono_jit_init_version` *after* apkenv's,
  with chaining enabled → Mono's runs first, chains to apkenv's for non-managed faults. Good. But
  `debug.c` sets `SA_ONSTACK`? and Mono with `--with-sigaltstack=no` doesn't → fine. Confirm nothing
  in `debug.c` re-installs handlers later (grep) — if it does, it must not clobber Mono's.
- The `apkenv_my_sigaction` translation (bionic layout) applies only to *bionic* callers; native
  Mono calls glibc `sigaction` directly — no translation. Ensure the hook table's `sigaction` entry is
  not reached by host code (it isn't: host code links glibc symbols normally).

Build with `./build-webos.sh`; static check: `nm apkenv | grep hostlib`.

**Checkpoint D:** `APKENV_HOST_MONO=libs/webos/libmono-webos.so ./apkenv templerun2.apk` on device
logs `[HOSTLIB] libmono-webos.so: 118/118 symbols bridged`, `libmono.so` is *not* in the
`/proc/self/maps` dump, and the run gets **past** `mono_jit_init_version` (the `[UN] unityAndroidInit
done` line, or a *different* failure than yesterday's ASCII-pc SIGSEGV).

### 3.5 Contract review of what libunity asks next (rest of the day; playbook mode)
Now we are back in playbook territory: instrument, one theory at a time.
- Enable `APKENV_TRACE_CALLS=libmono:mono_jit_init_version,libmono:mono_domain_assembly_open,libmono:mono_runtime_invoke`
  — the tracer must forward to the **hook table address** for bridged symbols, not `apkenv_lookup_in_library`
  (which would find nothing / the wrapper itself — this is yesterday's recursion bug; fixing it is
  part of this stage: resolve bridged names via `dlsym` on the host handle, skip entries whose
  address equals the wrapper).
- `MONO_LOG_LEVEL=debug MONO_LOG_MASK=asm,dll,gc` in the env → native Mono's own tracing on stderr.
  This is the biggest new observability win: **Mono now tells us what it can't find.**
- Expected next contract gaps, in likely order: (1) `mono_file_map_override` callback semantics
  (libunity maps DLLs out of the apk — verify it's called and returns non-NULL for `mscorlib.dll`);
  (2) machine config path (`mono_register_machine_config`) fine; (3) `mono_set_dirs(lib, etc)`
  pointing inside `<dataDir>/lib` — must exist and be readable in the jail; (4) unity's plugin finder
  callback returning paths for `__Internal`; (5) first managed exception → `mono_runtime_unhandled_exception_policy_set`
  + Unity's `UnhandledException` path calling back into libunity.
- Stop condition for the day: either the first `nativeRender()` frame is reached (log
  `[UN] unityAndroidPrepareGameLoop done`), or a clean, single, reproducible next blocker is written
  into §6 with its evidence.

**Checkpoint E:** a log where `unityAndroidInit` returns, in `plan/logs/tr2-hostmono-1.log`.

## 4. Operating rules for Opus (from the playbook + memory)
- Evidence before theories. Every stage above ends in a checkpoint that is a *file* (log, script,
  binary check). No "it should work" transitions.
- No brute force on device: one build → one run → read the whole log → one change. Keep the
  `[UN-JNI]`/`[UN-CHK]`/`[HOSTLIB]` tracers on always.
- Never touch the pristine apk (`android-candidates/templerun2_1.2.1.apk`). Extract to scratchpad.
- Device access is via `novacom` (`build-webos.sh` shows the pattern); harness `/var/apkenv2/play-tr2.sh`,
  log `/media/internal/apkenv-tr2.log`. If no device is connected, complete **§3.1, §3.2, §3.4 (code +
  host build), and the §3.3 program** offline and leave a run protocol for the user.
- Keep the mono build reproducible: script + patch file checked in; the mono checkout stays in the
  scratchpad (or `apkenv/third_party/mono` if the user prefers — ask *after* the work, not before).
- Commit at each checkpoint with a message naming it (`TR2 Mono: checkpoint B — native libmono builds`).
- Update `plan/TEMPLERUN2.md` "Where it dies now" and `apkenv/BUILD-STATE.md` at the end; append
  new general learnings (anything not Mono-specific) to `PORTING-PLAYBOOK.md`.

## 5. Out of scope today (next stages, in order)
GLES version selection for Unity 3.5 (`settings.xml`/player settings) → GLES2-capable portrait FBO
present (shader blit; today's FBO path is GLES1-only) → FMOD pump verification → Prime31 `__Internal`
stubs → packaging as `com.apkenv.templerun2`.

## 6. Running log (Opus appends; newest last)
- 2026-08-27 (Fable): plan written. TLS theory refuted by byte scan (see §1). Branch `unity3.5` = 2.6.5 / corlib 82.
- 2026-08-27 (Opus) **CHECKPOINT A MET.** Cloned `Unity-Technologies/mono` branch `unity3.5` @ **`64c3378a67376d089f8ad6f7b6cad4619fdaefa9`** (matches the pinned ls-remote). **All 118 imports present — zero MISSING.** Audit script + results in scratchpad (`audit-found.txt`). Locations of the interesting ones:
  - `mono_file_map_override` → `mono/utils/mono-filemap.c:33`, **guarded by `#if defined(ANDROID)`**.
  - `mono_set_find_plugin_callback` → `mono/metadata/loader.c:1221`.
  - `mono_thread_suspend_all_other_threads` → `mono/metadata/threads.c:3152` (my first grep pattern just missed same-line return types).
  - `mono_unity_set_embeddinghostname`, `mono_unity_socket_security_enabled_set` → **`unity/unity_utils.c:90,113`** — a *top-level* `unity/` dir, not under `mono/`.
- 2026-08-27 (Opus) **Two build-wiring issues found in §3.1, to fix in §3.2** (neither was in the plan):
  1. **`unity/unity_utils.c` is not in the autotools build.** No `unity/Makefile.am`, `unity/` absent from `SUBDIRS` (Unity built it with their own system). It is not optional: `mono/metadata/icall-def.h:245,252` reference `mono_unity_get_embeddinghostname` / `mono_unity_socket_security_enabled_get` **unconditionally**, so libmono will not link without it. Fix: add `unity_utils.c` to the metadata sources. (`unity/unity_cross_utils.c` is 100% commented out — skip it.)
  2. **`mono_file_map_override` needs the `ANDROID` guard removed**, not `-DANDROID`. Blast radius check says a global `-DANDROID` is wrong: it also flips `libgc/include/private/gcconfig.h:2435` (Android GC config) and `libgc/pthread_stop_world.c:363` (Android-kernel errno tolerance), neither of which we want on webOS's kernel. Fix: unconditional in `mono-webos.patch`.
  - Watch item: `mono_unity_write_to_unity_log` does `fprintf(stdout, mono_string_to_utf8(str))` — non-literal format; may trip `-Werror=format-security` on gcc 4.3.3.
- 2026-08-27 (Opus) **Correction to the above:** issue #1 was WRONG. `../../unity/unity_utils.c` is *already* listed in `mono/metadata/Makefile.am:65` (`libmonoruntime_la_SOURCES`). Nothing to do; automake only warns about `subdir-objects`. The format-security watch item never fired either.
- 2026-08-27 (Opus) **Found Unity's own recipe in the tree: `build_runtime_android.sh`.** It is the authoritative flag set for this exact runtime, and I adapted it rather than guessing. Kept: `--disable-mcs-build --disable-parallel-mark --with-sigaltstack=no --with-tls=pthread --with-glib=embedded` and the armv7a CPU flags `-march=armv7-a -mfloat-abi=softfp -mfpu=vfp -DARM_FPU_VFP=1 -DHAVE_ARMV6=1`, LDFLAGS `-Wl,--fix-cortex-a8`. Dropped every Android-specific define (`-DANDROID -DPLATFORM_ANDROID -DPAGE_SIZE=0x1000 -DS_IWRITE=S_IWUSR -D_POSIX_PATH_MAX=256 …`) — those are bionic workarounds and are the whole point of not doing this. **Flipped `mono_cv_uscore` from Unity's `yes` to `no`**: it is an `AC_TRY_RUN` (so a cross build cannot answer it and it MUST be passed explicitly) controlling whether `dlsym` gets a `_` prefix. ELF/glibc has no prefix; Unity's `yes` is a bionic quirk, and getting it wrong would break `__Internal` P/Invokes.
- 2026-08-27 (Opus) **Build blockers hit and fixed (2, both now in `apkenv/tools/mono-webos.patch`):**
  1. `runtime/Makefile.am:2` `AUTOMAKE_OPTIONS = cygnus` — removed in automake ≥1.13, so `autoreconf` failed. It is only a hack to stop `check` depending on `all`; commented out. (autoconf 2.71 / automake 1.16.5 otherwise handle this 2009 tree fine — warnings only.)
  2. `mono/utils/mono-filemap.c` — dropped the `#if defined(ANDROID)` around `mono_file_map_override`, as planned.
- 2026-08-27 (Opus) **The one real compile blocker, worth remembering (general, not Mono-specific):** linking `genmdesc` died with `multiple definition of realpath / fgets / gets / stpncpy / wcstombs / …`. Cause: `eglib/src/Makefile.am` hardcodes `-D_FORTIFY_SOURCE=2`, which pulls in glibc **2.5**'s `bits/stdio2.h`/`string3.h`/`stdlib.h`; those use a bare **`extern __always_inline`** (the pre-2.7 spelling, no `__extern_always_inline` in this `sys/cdefs.h`). Under gcc 4.3's default **`-std=gnu99`** that emits an external definition in *every* TU. Fix: **`-fgnu89-inline`** in CFLAGS — restores gnu89 extern-inline semantics without touching the rest of C99. Reproduced and verified in isolation (5 stray wrappers → 0). *This will bite any old-glibc cross-build under this toolchain, not just Mono.* Note `genmdesc` itself never needs to RUN: `configure` correctly picks `GENMDESC_PRG = perl genmdesc.pl` via `AM_CONDITIONAL(CROSS_COMPILING)`; only its link was failing.
- 2026-08-27 (Opus) **CHECKPOINT B MET.** `hostlibs/webos/libmono-webos.so` (2.7 MB stripped; unstripped `build/webos/libmono-webos.so.debug` for addr2line). Zero build errors. Gate `tools/check-mono-exports.sh`: *all 118* libunity imports exported. `file` → ARM EABI5 shared object; **max symbol version GLIBC_2.4**; NEEDED = `librt libdl libpthread libstdc++ libm libc libgcc_s` (all present on the TouchPad — `libstdc++.so.6` to re-confirm on device in §3.3).
  - **Strong pin validation, better than the plan asked for:** the dynamic export set of our glibc build is **identical to the apk's bionic `libmono.so`** — 910 symbols each, `comm` difference **zero in both directions**, version string `2.6.5`. `unity3.5 @ 64c3378a` is exactly the tree Unity shipped in Temple Run 2.
  - Recipe codified: `apkenv/tools/build-mono-webos.sh` (clone→pin→patch→autoreconf→configure→make→strip→verify) + `tools/mono-webos.patch` + `tools/check-mono-exports.sh`.
  - **Host libs live in `apkenv/hostlibs/<platform>/`, NOT `libs/<platform>/`** (new dir + README). `libs/webos/` is `APKENV_LOCAL_BIONIC_PATH`, the *bionic* linker's search path, and `packaging/build-ipk.sh:97` copies `*.so` out of it into the jail's bionic lib dir — a glibc object must never land there.
- 2026-08-27 (Opus) **§3.4 host-lib bridge: CODE COMPLETE, apkenv builds** (ARM, max GLIBC_2.4). Pieces:
  - `compat/hostlib.[ch]` — generic: `apkenv_hostlib_bridge(path, soname, symbols, n)` `dlopen`s a host lib, `dlsym`s each name, `register_hooks()`. A missing symbol is **fatal at bridge time** by design (a half-bridged runtime would silently fall back to the bionic copy and corrupt state far away).
  - `compat/mono_symbols.h` — generated by `tools/gen-mono-hooklist.sh` from the committed import list (118 entries), so it cannot drift from the binary.
  - `compat/hooks.c` — `HOOKS_MAX` 1024→2048; new `BUILTIN_LIB_MONO`, but `get_builtin_lib_handle()` only returns it **when a host lib actually provides that SONAME**, so WMW/PvZ/Alex are untouched.
  - `apkenv.c` — bridge runs right after `hooks_init()` and **before any apk lib is dlopen'd** (relocation consults the hook table first, so it must be registered by then); gated on `APKENV_HOST_MONO=<path>`; blacklist entry for `libmono.so` added dynamically only when bridged.
- 2026-08-27 (Opus) **Two bridge-interaction bugs found and fixed that the plan did not anticipate** (both would have wasted a device run):
  1. **The tracer would have silently swallowed every traced Mono call.** `trace_call()` resolved only via `apkenv_find_library()`/`apkenv_lookup_in_library()` — the *bionic* namespace. With the bridge active `libmono.so` is not loaded there at all, so it returns NULL, prints "unresolved", and **returns NULL instead of calling anything** — indistinguishable from the runtime hanging. Now tries `apkenv_hostlib_dlsym()` first, then the bionic symtab. (Also added the guard the plan asked for: if a name resolves back to the tracer's own wrapper, drop it rather than recurse — yesterday's hang.)
  2. **Tracer and bridge would register duplicate hook names.** `hooks_init()` (tracer) runs before the bridge; both would add e.g. `mono_jit_init_version`, and `bsearch()` would pick one arbitrarily. The bridge now skips any name already hooked, so the tracer deterministically wins and forwards through `apkenv_hostlib_dlsym()`.
- 2026-08-27 (Opus) **Another plan item that turned out to be a no-op:** §3.4 said to skip `libmono`'s `JNI_OnLoad`. The apk's `libmono.so` **exports no `JNI_OnLoad` at all** (`nm`), and `modules/unity.c` only stores the lookup result — it never calls it. Left in place with a comment.
- 2026-08-27 (Opus) **§3.3 `tools/monotest.c` written and cross-compiled** (`build/webos/monotest`, ARM, GLIBC_2.4). It `dlopen`s the runtime (not links — matches what the bridge does, and dodges the `libmono.so.0` SONAME) and checks, in order: all symbols resolve → `mono_register_machine_config` + `mono_set_dirs`/`mono_set_assemblies_path` → **`mono_jit_init_version`** (where the bionic build dies) → `mono_get_corlib` → GC alloc + string round-trip → **JIT and run `System.Environment.get_TickCount`** → `Environment.Version.ToString()` → `mono_jit_cleanup`.
- 2026-08-27 (Opus) **BLOCKED on device access — Checkpoints C, D, E not reached.** A TouchPad *is* connected (`0830:8072`, `topaz-linux`) and `novacom -l` worked at the start of the session, but the **host `novacomd` is now wedged**: my first `novacom run` hung holding the session, and after killing every stale client `novacom -l` still times out (tested to 90 s). Recovery needs `sudo systemctl restart novacomd`, and sudo requires a password here.
  - **Everything is staged and one command from running.** `tools/tr2-device-run.sh c` (Checkpoint C) and `… d` (Checkpoint D) push the runtime, the Managed assemblies (extracted from the pristine apk into `build/webos/tr2-managed/`), and the binaries to `/var/apkenv2/`, then run them.
  - **Run protocol for the next session (in order):**
    1. `sudo systemctl restart novacomd` then confirm `novacom -l` returns the device.
    2. `apkenv/tools/tr2-device-run.sh c` → expect `[monotest] PASS`. Save to `plan/logs/tr2-monotest-1.log`. Failure triage is in §3.3 (corlib-82 mismatch → wrong pin; `GC_init` SIGSEGV → Boehm; SIGILL → VFP flags). Also confirm `libstdc++.so.6` resolves on device — it is a new NEEDED that the bionic libmono did not have.
    3. Only if C passes: `apkenv/tools/tr2-device-run.sh d`, then the printed `APKENV_HOST_MONO=… MONO_LOG_LEVEL=debug MONO_LOG_MASK=asm,dll,gc ./apkenv …` line. Expect `[HOSTLIB] libmono.so -> …: 118/118 symbols bridged`, **no `libmono.so` in the `/proc/self/maps` dump**, and either `[UN] unityAndroidInit done` or a *different* failure than the ASCII-`pc` SIGSEGV.
    4. Then §3.5 with `MONO_LOG_LEVEL=debug` — native Mono now reports what it cannot find, which is the single biggest new source of evidence.
- 2026-08-27 (Opus) **Build script validated end-to-end from a clean clone** (fresh `MONO_SRC_DIR`, real network clone → checkout pin → patch → autoreconf → configure → make → strip → verify, all green). Same result: GLIBC_2.4, 910 exports, all 118 imports, and the export set is **identical** to the first build (0-line diff). Not byte-reproducible — Mono bakes a build timestamp into `buildver.h` (`mono/mini/Makefile.am`), so the stripped `.so` differs by ~148 bytes between runs. Functionally equivalent; do not chase it.
- 2026-08-27 (Opus) **`RTLD_GLOBAL` safety checked** (the bridge publishes all 910 symbols into the global namespace). Non-`mono_*`/`g_*` exports are only 6 `GC_*` plus `VER_1`, and there are **zero** collisions with libc names (`malloc/free/read/write/mmap/sigaction/pthread_create/dlopen/…`). The only theoretical overlap is embedded eglib's `g_*`, and apkenv links no glib. Safe.
- 2026-08-27 (Opus) Verified offline, since the device is unreachable: `hooks_init()` sets `hooks_count` by scanning to the first NULL name *then* sorts, so the bridge's later `register_hooks()` append+re-sort is correct; none of the 118 names contain "pthread" (so no spurious `Unimplemented:` lines); worst-case table use is ~911 of `HOOKS_MAX` 2048.
- 2026-08-27 (Opus) **Device back** (user restarted `novacomd` + rebooted the tablet). `novacom run`/`put` all fine — the earlier hang really was the wedged daemon, not the syntax.
- 2026-08-27 (Opus) **Bug in my own `tools/tr2-device-run.sh`, fixed:** `nc_run()` already appends the `--` separator and the call sites appended a second one, so the target received a literal `--` as argv[1]; busybox then treated the real flags as operands. It surfaced as `mkdir: can't create directory '-p': Read-only file system` — the giveaway being that `novacom run`'s cwd is `/`, which webOS mounts **ro** (`/var` is rw, `/media/internal` is rw).
- 2026-08-27 (Opus) **CHECKPOINT C MET — native Mono runs on webOS.** `plan/logs/tr2-monotest-1.log`, HP TouchPad, first real run, every stage green:
  `dlopen OK` (so the new `libstdc++.so.6`/`librt`/`libgcc_s` NEEDEDs all resolve on device) → all symbols resolved → **`mono_jit_init_version` OK** (`domain=0x2b0bfe70`) → `corlib=0x16608` → GC alloc + string round-trip → **the JIT compiled and ran managed code: `Environment.TickCount = 162820`** → reflection + instance invoke: `Environment.Version = 3.0.40818.0` → `mono_jit_cleanup` → `PASS`.
  - **This is the decisive result of the native-Mono track.** `mono_jit_init_version` is the exact call the bionic build died inside (the ASCII-`pc` SIGSEGV). JIT, Boehm GC, signal setup, thread attach and Unity's own corlib are all fine on webOS once Mono is built against the device's glibc. Everything from here is bridge and libunity-contract work, not runtime work.
- 2026-08-27 (Opus) **A gap in this plan's own §3.4, found on the first bridged run:** the symbol list was generated by matching the `mono_` prefix, but libunity also resolves **`g_free`** (embedded eglib) and **`GC_delete_thread` / `GC_lookup_thread`** (Boehm) out of libmono. The run died at `linker.c:1383| ERROR: 0 cannot locate 'g_free'... failed to link libunity.so`. The correct derivation is **{libunity's UNDEFINED symbols} ∩ {libmono's DEFINED symbols} = 121**, and `tools/gen-mono-hooklist.sh` now does exactly that (all 121 are already exported by our build). Do not go back to prefix matching.
  - Footgun while fixing it: `build-webos.sh` rebuilds an object only when the **`.c`** is newer than the `.o` — it does not track header dependencies, so regenerating `compat/mono_symbols.h` alone silently relinked the old 118-symbol table. `touch` the `.c` after changing a generated header.
- 2026-08-27 (Opus) **CHECKPOINT D MET.** `plan/logs/tr2-hostmono-2.log`:
  - `[HOSTLIB] libmono.so -> /var/apkenv2/libmono-webos.so: **121/121 symbols bridged**`
  - libunity linked and loaded; the apk's bionic `libmono.so` is **absent from the `/proc/self/maps` dump**.
  - The whole Java host contract ran: `JNI_OnLoad(libunity)` → `nativeFile(apk)` → `initJni` → `InitPlayerPrefs` → `nativeInit(1024,768)` → `unityAndroidInit(assets/bin/, …/lib)`.
  - **Mono is running natively inside apkenv:** `Mono: gc took 315 usecs`, `Assembly Loader probing location: …/Managed/mscorlib.dll`, `Assembly Loader loaded assembly`, `Assembly mscorlib 0x138548 added to domain Unity Root Domain, ref_count=1`.
  - **We are past `mono_jit_init_version`.** Yesterday's ASCII-`pc` SIGSEGV inside Mono init is gone, and with it the whole bionic-ABI corruption class. `MONO_LOG_LEVEL=debug` now narrates what the runtime loads — exactly the new evidence source the plan predicted.
- 2026-08-27 (Opus) **NEXT BLOCKER — clean, reproducible, and precisely located** (§3.5 stop condition). `plan/logs/tr2-hostmono-3.log`:
  ```
  [UN-JNI] UNHANDLED int GetInt(Ljava/lang/String;I)I
  [UN-JNI] UNHANDLED int getTotalMemory()I
  [PTHREAD] >>> start tid=863409264 routine=0x2c4bd364
  [UN-JNI] GetStringUTFChars on GLOBAL ref -> NULL      <-- the cause
  signal 11 addr=(nil)  pc in /lib/libc.so.6 (strlen)  lr in libunity.so +0x70dd0
  ```
  `modules/unity.c:unity_jnienv_GetStringUTFChars()` returns **NULL** when the jstring equals the global ref. libunity feeds that straight into a `std::string(const char*)` ctor — disassembly at `libunity+0x70db4` is `r0=r1=src; bl strlen; r7 = src + len; …` — so `strlen(NULL)` faults with `r0=r1=r6=0`. Confirmed by instrumentation, not inferred: the new log line lands immediately before the fault.
  - Instrumentation kept: that NULL return (and a NULL `str->data`) now log loudly, so this can never again present as a context-free SIGSEGV in `strlen`.
  - **Do not just return `""`.** That hides the real question: *why is libunity calling `GetStringUTFChars` on the global (UnityPlayer/activity) object instead of a String?* Something upstream handed it the global ref where a `java.lang.String` was expected. The playbook answer is to read `UnityPlayer.smali` for the calls made right after `mscorlib` loads (`PlayerPrefs.GetInt`, `getTotalMemory` are the two unhandled ones immediately before) and derive which one should return a String. Both unhandled methods return `int`, so neither is directly the NULL source — the handoff is elsewhere, most likely a `CallObjectMethod`/field read that yields the global.




## A. The 118 imports (also `plan/tr2-mono-imports.txt`)
mono_add_internal_call mono_array_class_get mono_array_new mono_array_new_full
mono_assembly_fill_assembly_name mono_assembly_foreach mono_assembly_get_image mono_assembly_loaded
mono_assembly_load_from_full mono_assembly_name_parse mono_class_array_element_size
mono_class_enum_basetype mono_class_from_mono_type mono_class_from_name mono_class_get
mono_class_get_field_from_name mono_class_get_fields mono_class_get_flags mono_class_get_image
mono_class_get_method_from_name mono_class_get_methods mono_class_get_name mono_class_get_namespace
mono_class_get_parent mono_class_get_type mono_class_instance_size mono_class_is_enum
mono_class_is_generic mono_class_is_inflated mono_class_is_subclass_of mono_class_vtable
mono_custom_attrs_free mono_custom_attrs_from_class mono_custom_attrs_from_field
mono_custom_attrs_from_method mono_custom_attrs_has_attr mono_domain_assembly_open mono_domain_get
mono_exception_from_name_msg mono_field_get_flags mono_field_get_name mono_field_get_offset
mono_field_get_type mono_field_get_value mono_field_set_value mono_file_map_override mono_gc_collect
mono_gchandle_free mono_gchandle_get_target mono_gchandle_new mono_gchandle_new_weakref
mono_gc_max_generation mono_get_boolean_class mono_get_byte_class mono_get_char_class
mono_get_corlib mono_get_double_class mono_get_enum_class mono_get_exception_class
mono_get_int16_class mono_get_int32_class mono_get_int64_class mono_get_object_class
mono_get_root_domain mono_get_single_class mono_get_string_class mono_image_close
mono_image_get_assembly mono_image_get_table_rows mono_image_open_from_data_full
mono_image_open_from_data_with_name mono_jit_cleanup mono_jit_init_version
mono_metadata_signature_equal mono_method_get_class mono_method_get_name mono_method_signature
mono_object_get_class mono_object_new mono_object_new_alloc_specific
mono_parse_default_optimizations mono_raise_exception mono_register_machine_config
mono_runtime_delegate_invoke mono_runtime_invoke mono_runtime_object_init
mono_runtime_set_shutting_down mono_runtime_unhandled_exception_policy_set
mono_security_set_core_clr_platform_callback mono_security_set_mode mono_set_assemblies_path
mono_set_commandline_arguments mono_set_defaults mono_set_dirs mono_set_find_plugin_callback
mono_set_signal_chaining mono_signature_get_param_count mono_signature_get_params
mono_signature_get_return_type mono_signature_is_instance mono_string_from_utf16
mono_stringify_assembly_name mono_string_new_wrapper mono_string_to_utf8 mono_thread_attach
mono_thread_current mono_thread_detach mono_thread_pool_cleanup mono_thread_set_main
mono_threads_set_shutting_down mono_thread_suspend_all_other_threads mono_type_get_class
mono_type_get_name mono_type_get_object mono_type_get_type mono_unity_set_embeddinghostname
mono_unity_socket_security_enabled_set mono_verifier_set_mode

## 7. After the bridge: getting pixels on screen (2026-08-27, same session)

Checkpoint D put Mono and the Java contract right; everything below is what stood
between "the engine runs" and "the game draws". Each was found by measurement.

1. **`GetStringUTFChars` on the global ref → `strlen(NULL)`** — *root cause:*
   `modules/unity.c` overrode `CallObjectMethod` but **not `CallObjectMethodV`**, so the
   va_list form fell through to `jni/jnienv.c`'s generic fallback, which returns
   `GLOBAL_J(env)` (the GlobalState pointer, used as a sentinel) for *every* unanswered
   object call. libunity fed that to `GetStringUTFChars`, got NULL, and built a
   `std::string` from it. Fixed by routing both variants through one dispatch.
   **Lesson: override the `V` variant of every `Call*Method` you override.**
2. **PlayerPrefs** — managed `PlayerPrefs.SetX()` **throws** `PlayerPrefsException` when the
   Java side returns false, which was aborting `AudioManager.Awake()`,
   `GameController.Awake()` and `Promotion.BeginPromo()`. Implemented as a real
   persistent store (`<dataDir>/playerprefs.txt`).
3. **`dlopen("/system/lib/libEGL.so")` failing ~11x/frame** (13,498 errors in one run) —
   `get_builtin_lib_handle()` compared the *whole string* against `"libEGL.so"`. Engines
   use absolute Android paths. Now matches on **basename**.
4. **The missing first-frame contract.** `UnityPlayer.onDrawFrame()`'s first-time tail is
   `unityAndroidInit → unityAndroidPrepareGameLoop → nativeResize(w,h,w,h) → nativeResume()
   → windowFocusChanged(true)`. We called neither `nativeResize` (signature `(IIII)V`, read
   from libunity's JNINativeMethod table and confirmed in `UnityPlayer.smali`) nor
   `nativeResume`. Order matters: `nativeResize` next to `nativeInit` is **too early** —
   the graphics device does not exist yet and the size is dropped.
5. **THE renderer bug: duplicate GLES hook names.** `gles_mapping.h` and `gles2_mapping.h`
   share **68 names** (`glClear`, `glDrawArrays`, `glViewport`, `glBindTexture`, ...). An
   engine that links both libs — Unity does — makes apkenv register both tables;
   `register_hooks()` appended duplicates and `apkenv_get_hooked_symbol()` `bsearch`ed a
   table with two entries per name, getting an **arbitrary** one. Half the engine's calls
   went to the ES1 wrapper and half to the ES2 wrapper, against a single ES1 context:
   ~10,200 draw calls and 14.5M vertices a second, correct 1024x768 viewport, **blank blue
   screen**. `register_hooks_nodup()` keeps the first (DT_NEEDED order puts
   `libGLESv1_CM.so` first, which is what we want). **This produced the title screen.**
   - *Method note:* my first probe instrumented only the ES1 wrappers and reported
     "0 draws, glViewport never called". That conclusion was **wrong** — the calls were
     going to the ES2 table. Probing one of two possible dispatch paths is not a
     measurement. Instrument every path a symbol could take before concluding.
6. **ETC1 textures rejected** — the game ships `GL_ETC1_RGB8_OES` (0x8D64) textures, and the
   Adreno 220's GLES1 context does not expose `GL_OES_compressed_ETC1_RGB8_texture`:
   `glCompressedTexImage2D` returned `GL_INVALID_ENUM` (0x500) for every texture, so all
   uploads were dropped and geometry drew untextured (flat purple silhouettes).
   `compat/etc1.c` decodes ETC1 on the CPU and uploads RGB8. GL errors after upload: 0.

### GLES2 is not available through SDL on this device — do not retry
`webos://knowledge/opengl-es-on-touchpad` and `webos://knowledge/pdk` both document it, and
this session re-confirmed it: asking SDL for an ES2 context yields
`Could not create EGL context` for every format and size, because Palm's SDL requests
`EGL_CONTEXT_CLIENT_VERSION=2` / `EGL_RENDERABLE_TYPE=ES2_BIT` and the Adreno driver answers
`EGL_BAD_ALLOC`. The device reports `GL_VERSION "OpenGL ES-CM 1.1"` through this path. Using
raw EGL to get ES2 would break the 3-layer compositor (touch flicker) — the KB's explicit
"never call EGL directly" rule. `platform/webos.c` now **falls back to ES1** rather than
failing to start, and `apkenv.c` gained a module GLES preference + `APKENV_GLES_VERSION`.
Fortunately Unity 3.5's libunity carries **both** renderers (11 fixed-function imports and 14
shader imports) and picks fixed-function here — `glCreateShader` is never called.

### Open
- **Touch does nothing on the title screen.** `nativeTouch` signature `(IFFIJI)V` matches the
  module's typedef exactly and `module->input` is wired, so the shape is right; the open
  questions are whether the events arrive (`[SDLHB]` showed `ev_total=1`) and whether Unity
  wants pixels or another coordinate space. Next: log every `unity_input()` call, and read
  `UnityPlayer.smali`'s `dispatchTouchEvent`/`onTouchEvent` for what the Java host really sends.
- Verify the ETC1 decode visually; check for remaining colour/format issues.
- Then: audio verification, portrait/orientation, packaging as `com.apkenv.templerun2`.
