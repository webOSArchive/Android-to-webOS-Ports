# Temple Run 2 (Unity 3.5.7f6 + Mono) — open track (2026-08-26)

Status: **not running**. Boots through the whole Java `UnityPlayer` init sequence and dies inside
Mono initialization. This is a different class of port from WMW/PvZ/Alex: the blocker is not the
Java host contract but **bionic↔glibc ABI mismatches at the libc layer** under apkenv. Each one is
a silent memory corruption, so progress is by evidence (crash handler + tracers), not theories.

## Anatomy
- Real libs are `assets/libs/armeabi-v7a/{libunity.so 6.8MB, libmono.so 3.8MB}` (the `lib/` copies
  are stubs); apkenv's lib-dir order already prefers `assets/libs`.
- Game logic = C# in `assets/bin/Data/Managed/*.dll` run by the Mono JIT (anon RWX works here).
- Both GLES1 and GLES2 symbols; build defaults to GLES1 when both are present (`apkenv.c`) —
  Unity 3.5 will want whichever `settings.xml`/player settings say; unverified.
- Portrait game (needs the render-to-FBO path; that path is GLES1-only today).
- Audio = FMOD via `org.fmod.FMODAudioDevice` → `audio/fmod_pump.c` (already wired in `unity.c`).
- Java host contract: `modules/unity.c` now follows `UnityPlayer.smali` order —
  `nativeFile(apk)` → `initJni` → `InitPlayerPrefs` → `nativeInit(w,h)` →
  `unityAndroidInit("assets/bin/", <dataDir>/lib)` → `unityAndroidPrepareGameLoop` →
  `nativeRender()` per frame; touch = `nativeTouch(pointerId, x, y, action&0xff, eventTime, 0)`.
  Handlers for `getPackageCodePath/getFilesDir/getCacheDir/getPackageName/getCPUType/
  getDeviceUniqueIdentifier/getScreenDPI/getDeviceOrientation` + `[UN-JNI]` unhandled tracer.
  Harness: `/var/apkenv2/play-tr2.sh`, apk `/media/internal/templerun2.apk`,
  log `/media/internal/apkenv-tr2.log`. Only `HashMap.put`/`toString` unhandled so far.

## General runtime bugs found and FIXED (benefit every port; in `apkenv/compat`, `debug`, `linker`)
1. **Second heap**: `memalign` (and `pvalloc`, `malloc_usable_size`) were not hooked, so a libunity
   constructor's `memalign` ran bionic libc.so's *own* dlmalloc; those pointers later hit glibc
   `free()` → `free(): invalid pointer` and cascading corruption (STLport free lists, a bad `FILE*`
   in `fseek`). Hooked the whole malloc family. Rule: **every allocator entry point must be hooked**.
2. **`struct sigaction` layout**: bionic/ARM = 16 bytes (4-byte `sa_mask`), glibc = 140 bytes.
   `sigaction`/`pthread_sigmask`/`sigprocmask` were mapped straight to glibc → `oldact`/`oset`
   writes smash the caller's stack (Mono installs many handlers at init). Now translated
   (`apkenv_my_sigaction` etc.). Rule: **any struct crossing the boundary needs a layout check**.
3. `__pthread_cleanup_push/pop` mapped to the fatal `no_hook` stub → implemented (per-thread stack,
   bionic record layout). `__pthread_gettid`, `pthread_getcpuclockid` mapped to real impls.
4. `no_hook` now reports the caller (`lib+offset`); "Unimplemented: X" lines are informational
   (pthread-named symbols not in the hook table, resolved from the libs themselves).
5. Crash handler (`debug/debug.c`): malloc-free (`backtrace_symbols_fd`), thread-safe (only the first
   faulting thread scans), prints signal/pc/**registers/lr** first, raw `/proc/self/maps` dump before
   unwinding, resolves anonymously-mapped bionic libs via `apkenv_android_dladdr` (made lock-free
   from a handler: `apkenv_dladdr_nolock`, else it deadlocks on the linker lock during constructors),
   stack scan capped at 2048 words. `stdout` line-buffered so logs interleave in order.
6. Tools: `APKENV_TRACE_FREE=<ptr>` (who frees a given pointer), `APKENV_TRACE_CALLS=lib:sym,…`
   (log entry/exit + 4 args of named engine→runtime calls; forwards to the library's real symbol
   via `apkenv_find_library` + `apkenv_lookup_in_library`).

## Where it dies now
`unityAndroidInit` → Mono init: `SIGSEGV pc=0x3332xxxx` — pc is **ASCII digits** (varies per run:
"8423", "4233"), i.e. a function pointer overwritten with text that looks like `/proc/self/maps`
content (Boehm GC's `GC_get_maps` reads it at init). `lr` = glibc `__libc_malloc+0xdc` (stale: the
last `bl` was malloc's `kuser_cmpxchg` lock) so the bad jump is a tail-call/`ldr pc` without a link.
`__malloc_hook` etc. are NULL right before the call (`[UN-CHK]`), and a 200µs watchdog never saw
them change — so it is not the glibc hooks.
Traced Mono call order from libunity: `mono_file_map_override`, `mono_register_machine_config`,
`mono_set_dirs`, `mono_set_assemblies_path`, `mono_set_signal_chaining(1)`,
`mono_parse_default_optimizations`, `mono_set_defaults`, `mono_set_commandline_arguments`,
`mono_jit_init_version` (crash somewhere after this begins).

## Next steps (in order)
1. Fix the tracer recursion: the resolved "real" `mono_file_map_override` re-entered the tracer
   (likely resolved to a PLT/hook path) — resolve via the lib's symtab value only, or skip entries
   whose address equals the wrapper; then bracket which Mono call corrupts memory.
2. Remaining suspect ABI mismatches to audit before more runs (same class as #1/#2 above):
   `pthread_attr_t` (bionic 24 B vs glibc 36 B — `pthread_getattr_np`/`pthread_attr_init` are hooked
   to glibc and WRITE into Mono's smaller struct; Mono calls them at thread attach), `sem_*`
   (unhooked → bionic, consistent), `sigsetjmp/siglongjmp` (unhooked → bionic, consistent),
   `struct stat` sizes, `dirent`, `ucontext` seen by handlers.
3. Then: GLES version choice for Unity, FBO present for a GLES2 context (shader blit), FMOD.

Estimate: a multi-session project (bionic-ABI translation layer, then GLES2 portrait, then audio).
