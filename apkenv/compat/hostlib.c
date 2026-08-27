/**
 * apkenv - host-library bridge (see hostlib.h for the rationale).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "hostlib.h"
#include "hooks.h"

#define HOSTLIB_MAX 4

struct hostlib {
    char *libname;      /* bionic SONAME we stand in for, e.g. "libmono.so" */
    char *path;         /* host .so actually loaded */
    void *handle;       /* host dlopen() handle */
};

static struct hostlib hostlibs[HOSTLIB_MAX];
static int hostlibs_count;

int
apkenv_hostlib_active(void)
{
    return hostlibs_count > 0;
}

int
apkenv_hostlib_provides(const char *libname)
{
    int i;

    if (libname == NULL)
        return 0;

    for (i = 0; i < hostlibs_count; i++)
        if (strcmp(hostlibs[i].libname, libname) == 0)
            return 1;

    return 0;
}

void *
apkenv_hostlib_dlsym(const char *libname, const char *sym)
{
    int i;

    if (libname == NULL || sym == NULL)
        return NULL;

    for (i = 0; i < hostlibs_count; i++)
        if (strcmp(hostlibs[i].libname, libname) == 0)
            return dlsym(hostlibs[i].handle, sym);

    return NULL;
}

int
apkenv_hostlib_bridge(const char *path, const char *libname,
                      const char *const *symbols, size_t n)
{
    struct _hook *table;
    void *handle;
    size_t i;
    size_t resolved = 0;
    size_t missing = 0;

    if (path == NULL || libname == NULL || symbols == NULL || n == 0) {
        fprintf(stderr, "[HOSTLIB] bad arguments\n");
        return -1;
    }

    if (hostlibs_count >= HOSTLIB_MAX) {
        fprintf(stderr, "[HOSTLIB] too many bridged libraries (max %d)\n",
                HOSTLIB_MAX);
        return -1;
    }

    /* RTLD_GLOBAL so the runtime's own symbols are visible to anything it
     * dlopen()s itself, and to P/Invoke-style lookups against the process. */
    handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        fprintf(stderr, "[HOSTLIB] dlopen(\"%s\") failed: %s\n", path, dlerror());
        return -1;
    }

    table = calloc(n, sizeof(struct _hook));
    if (table == NULL) {
        fprintf(stderr, "[HOSTLIB] out of memory\n");
        dlclose(handle);
        return -1;
    }

    for (i = 0; i < n; i++) {
        void *addr;

        if (symbols[i] == NULL)
            continue;

        /* Leave anything already hooked alone. In practice that is the call
         * tracer (APKENV_TRACE_CALLS), which registers in hooks_init() - i.e.
         * before us. Two entries with the same name would make bsearch() pick
         * one arbitrarily; letting the tracer win is both deterministic and
         * what the user asked for. The tracer forwards to us via
         * apkenv_hostlib_dlsym(). */
        if (apkenv_get_hooked_symbol(symbols[i], 0) != NULL) {
            fprintf(stderr, "[HOSTLIB] %s already hooked (tracer?) - not overriding\n",
                    symbols[i]);
            continue;
        }

        dlerror();
        addr = dlsym(handle, symbols[i]);
        if (addr == NULL) {
            /* A symbol that is legitimately NULL cannot be distinguished from
             * an absent one by the return value alone; ask dlerror(). */
            const char *err = dlerror();
            if (err != NULL) {
                fprintf(stderr, "[HOSTLIB] MISSING %s in %s\n", symbols[i], path);
                missing++;
                continue;
            }
        }

        table[resolved].name = symbols[i];
        table[resolved].func = addr;
        resolved++;
    }

    if (missing != 0) {
        /* Fatal by design - see hostlib.h. A partially bridged runtime would
         * silently fall back to the apk's bionic copy for the missing pieces
         * and corrupt state far from here. */
        fprintf(stderr, "[HOSTLIB] %s: %zu/%zu symbols bridged, %zu MISSING - aborting\n",
                libname, resolved, n, missing);
        free(table);
        dlclose(handle);
        return -1;
    }

    if (register_hooks(table, resolved) != 0) {
        fprintf(stderr, "[HOSTLIB] register_hooks() failed for %s "
                        "(raise HOOKS_MAX in compat/hooks.c)\n", libname);
        free(table);
        dlclose(handle);
        return -1;
    }

    /* register_hooks() copies the entries, but struct _hook stores the name as
     * a plain pointer - the caller's symbol strings must outlive us. They are
     * string literals in a generated table, so that holds. The table itself is
     * ours to free. */
    free(table);

    hostlibs[hostlibs_count].libname = strdup(libname);
    hostlibs[hostlibs_count].path = strdup(path);
    hostlibs[hostlibs_count].handle = handle;
    hostlibs_count++;

    fprintf(stderr, "[HOSTLIB] %s -> %s: %zu/%zu symbols bridged\n",
            libname, path, resolved, n);
    if (resolved != n)
        fprintf(stderr, "[HOSTLIB] (%zu left to existing hooks)\n", n - resolved);

    return 0;
}
