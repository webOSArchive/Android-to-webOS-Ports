#ifndef COMPAT_HOSTLIB_H
#define COMPAT_HOSTLIB_H

/**
 * apkenv - host-library bridge
 *
 * Loads a *host* (glibc) shared object with the real dlopen() and publishes a
 * named set of its symbols into apkenv's hook table, so that bionic code loaded
 * by apkenv's linker resolves those imports to the host build instead of to the
 * bionic library shipped inside the apk.
 *
 * Motivation: engines that carry their own language runtime (Unity/Mono, and
 * anything else with a JIT) do everything glibc-hostile at once - signal
 * handlers, GC stack scanning, thread attach, JIT page mmap/mprotect. Bridging
 * the whole runtime to a natively-built copy removes that entire class of
 * bionic-ABI corruption in one step, instead of translating it call by call.
 *
 * This is general infrastructure; nothing here is Mono-specific.
 */

#include <stddef.h>

/**
 * Load `path` with the host dynamic loader and register `symbols` (n entries,
 * NULL-terminated array not required) as hooks.
 *
 * `libname` is the bionic SONAME this host library stands in for (e.g.
 * "libmono.so"); it is reported in log lines and is what
 * apkenv_hostlib_provides() will answer to.
 *
 * Every requested symbol must resolve. A missing symbol is fatal *here*,
 * on purpose: if it were tolerated, the bionic library would silently supply
 * it later (or the relocation would fail deep inside engine init), which is
 * exactly the kind of half-bridged state that is impossible to debug.
 *
 * Returns 0 on success, -1 on failure.
 */
int apkenv_hostlib_bridge(const char *path, const char *libname,
                          const char *const *symbols, size_t n);

/** Non-zero once at least one host library has been bridged. */
int apkenv_hostlib_active(void);

/**
 * Non-zero if `libname` (a bare SONAME, e.g. "libmono.so") is supplied by a
 * bridged host library. Used to (a) satisfy DT_NEEDED without a file and
 * (b) blacklist the apk's own copy from being loaded.
 */
int apkenv_hostlib_provides(const char *libname);

/**
 * Look a symbol up in a bridged host library by SONAME. Returns NULL if the
 * library is not bridged or the symbol is absent.
 *
 * The call tracer must use this rather than apkenv_find_library() /
 * apkenv_lookup_in_library(): those search the *bionic* namespace, where a
 * bridged symbol either does not exist or resolves back to the tracer's own
 * wrapper (that self-recursion is what hung the tracer on the first Mono runs).
 */
void *apkenv_hostlib_dlsym(const char *libname, const char *sym);

#endif /* COMPAT_HOSTLIB_H */
