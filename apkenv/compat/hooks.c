
/**
 * apkenv
 * Copyright (c) 2012, Thomas Perl <m@thp.io>
 * Based on code from libhybris: Copyright (c) 2012 Carsten Munk
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **/

#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/uio.h>

#include "hooks.h"
#include "hostlib.h"
#include "../apkenv.h"
#include "../linker/linker.h"

#include "libc_wrappers.h"
#include "liblog_wrappers.h"
#include "egl_wrappers.h"
#include "gles_wrappers.h"
#include "gles2_wrappers.h"
#include "pthread_wrappers.h"
#include "linux_wrappers.h"
#include "android_wrappers.h"

#include "../debug/wrappers.h"

extern struct GlobalState global;

char my___sF[SIZEOF_SF * 3];

/* raised from 1024: the host-library bridge (compat/hostlib.c) adds a whole
 * language runtime's export list at once - Mono contributes 118. */
#define HOOKS_MAX 2048

static void no_hook(void);

static struct _hook hooks[HOOKS_MAX] = {
#include "libc_mapping.h"
#include "liblog_mapping.h"
#include "egl_mapping.h"
#include "pthread_mapping.h"
#include "linux_mapping.h"
#include "android_mapping.h"

  {"__sF", my___sF},
};
static int hooks_count;

#ifdef APKENV_GLES
static struct _hook hooks_gles1[] = {
#include "gles_mapping.h"
};
#define HOOKS_GLES1_COUNT (sizeof(hooks_gles1) / (HOOK_SIZE))
#endif

#ifdef APKENV_GLES2
static struct _hook hooks_gles2[] = {
#include "gles2_mapping.h"
};
#define HOOKS_GLES2_COUNT (sizeof(hooks_gles2) / (HOOK_SIZE))
#endif

/* fully wrapped or harmful libs that should not be loaded
 * even if provided by user (like 3D driver libs) */
enum builtin_library_id {
    BUILTIN_LIB_EGL = 0,
    BUILTIN_LIB_GLESV1 = 1,
    BUILTIN_LIB_GLESV2 = 2,
    /* Only builtin while a host library is standing in for it - see
     * get_builtin_lib_handle(). Otherwise the apk's own copy loads normally. */
    BUILTIN_LIB_MONO = 3,
};

static const char *builtin_libs[] = {
    [BUILTIN_LIB_EGL] = "libEGL.so",
    [BUILTIN_LIB_GLESV1] = "libGLESv1_CM.so",
    [BUILTIN_LIB_GLESV2] = "libGLESv2.so",
    [BUILTIN_LIB_MONO] = "libmono.so",
};

/* this is just to not log errors if those libs are missing */
static const char *optional_libs[] = {
    // "libc.so", // not yet
    // "libm.so", // not yet
    // "libstdc++.so", // not yet
    "liblog.so",
    "libz.so",
};

static int
hook_cmp(const void *p1, const void *p2)
{
    const struct _hook *h1 = (const struct _hook *)p1;
    const struct _hook *h2 = (const struct _hook *)p2;
    return strcmp(h1->name, h2->name);
}

#define HOOK_SIZE (sizeof(struct _hook))

void *apkenv_get_hooked_symbol(const char *sym, int die_if_pthread)
{
    struct _hook target;
    target.name = sym;

    struct _hook *result = bsearch(&target, &(hooks[0]),
            hooks_count, HOOK_SIZE, hook_cmp);

    if (result != NULL) {
        return result->func;
    }

    if (strstr(sym, "pthread") != NULL) {
        if(die_if_pthread)
        {
            printf("Unimplemented but required: %s\n", sym);
            exit(4);
        }
        printf("Unimplemented: %s\n", sym);
    }

    return NULL;
}

void *apkenv_get_hooked_symbol_dlfcn(void *handle, const char *sym)
{
    struct _hook *result;
    struct _hook target;
    target.name = sym;

    if (is_builtin_lib_handle(handle)) {
        enum builtin_library_id builtin_lib_id = (const char **)handle - builtin_libs;
#ifdef APKENV_GLES
        if (builtin_lib_id == BUILTIN_LIB_GLESV1) {
            result = bsearch(&target, hooks_gles1, HOOKS_GLES1_COUNT,
                HOOK_SIZE, hook_cmp);
            if (result != NULL)
                return result->func;
            return NULL;
        }
#endif
#ifdef APKENV_GLES2
        if (builtin_lib_id == BUILTIN_LIB_GLESV2) {
            result = bsearch(&target, hooks_gles2, HOOKS_GLES2_COUNT,
                HOOK_SIZE, hook_cmp);
            if (result != NULL)
                return result->func;
            /* A miss here can be the whole ballgame: an engine that probes for
             * an ES2 entry point and gets NULL concludes the device has no ES2
             * and builds its fixed-function renderer instead. */
            printf("GLES2 dlsym MISS: %s\n", sym);
            return NULL;
        }
#endif
    }

    return apkenv_get_hooked_symbol(sym, 1);
}

/* Register a GLES table, resolving the 68 names GLES1 and GLES2 share.
 *
 * gles_mapping.h and gles2_mapping.h both define glClear, glDrawArrays,
 * glViewport, glBindTexture, ... An engine that links both libs - Unity does -
 * makes apkenv register both tables. Plain register_hooks() appends duplicates,
 * apkenv_get_hooked_symbol() bsearch()es a table with two entries per name and
 * gets an ARBITRARY one, and half the engine's calls go to each wrapper against
 * one context: geometry submitted and silently never drawn (measured on Temple
 * Run 2: ~10k draw calls and 14M vertices a second, blank screen).
 *
 * The winner must be the table matching the context we ACTUALLY got, because
 * the two wrappers forward to two different device libraries (libGLES_CM.so vs
 * libGLESv2.so) and only one of them belongs to the live context. Registration
 * order is DT_NEEDED order, which puts GLES1 first regardless - so an explicit
 * priority is needed, not "keep the first".
 *
 * apkenv_active_gles_version() is set by the platform AFTER context creation,
 * so it reflects any fallback (webos.c degrades ES2 -> ES1 if EGL refuses). */
static int active_gles_version = 1;

static struct _hook *hook_entry(const char *name);

/* Re-point every name this table shares with the other one at THIS table.
 * Registration happens while the apk's libs are being resolved, which is
 * before the platform has created a context - so the first registration is a
 * guess. This is the correction, applied once the real context version is
 * known; the hook table is consulted at relocation time, which is later still. */
static void
gles_override_shared(const struct _hook *table, size_t count, int version)
{
    size_t i, changed = 0;
    for (i = 0; i < count; i++) {
        struct _hook *e = hook_entry(table[i].name);
        if (e != NULL && e->func != table[i].func) {
            e->func = table[i].func;
            changed++;
        }
    }
    if (changed != 0)
        printf("GLES: re-pointed %zu shared symbols at the ES%d wrappers "
               "(the live context)\n", changed, version);
}

void apkenv_set_active_gles_version(int v)
{
    if (v != 1 && v != 2)
        return;
    active_gles_version = v;

#ifdef APKENV_GLES
    if (v == 1) gles_override_shared(hooks_gles1, HOOKS_GLES1_COUNT, 1);
#endif
#ifdef APKENV_GLES2
    if (v == 2) gles_override_shared(hooks_gles2, HOOKS_GLES2_COUNT, 2);
#endif
}

int apkenv_active_gles_version(void)
{
    return active_gles_version;
}

static struct _hook *hook_entry(const char *name)
{
    struct _hook key;
    key.name = name;
    key.func = NULL;
    return (struct _hook *)bsearch(&key, hooks, hooks_count, HOOK_SIZE, hook_cmp);
}

static int register_hooks_gles(const struct _hook *new_hooks, size_t count, int table_version)
{
    struct _hook filtered[512];
    size_t i, n = 0, skipped = 0, replaced = 0;
    int preferred = (table_version == apkenv_active_gles_version());

    for (i = 0; i < count && n < sizeof(filtered) / sizeof(filtered[0]); i++) {
        struct _hook *existing = hook_entry(new_hooks[i].name);
        if (existing != NULL) {
            /* The table that matches the live context wins, whichever order the
             * engine's DT_NEEDED list registered them in. */
            if (preferred) { existing->func = new_hooks[i].func; replaced++; }
            else skipped++;
            continue;
        }
        filtered[n++] = new_hooks[i];
    }
    if (skipped != 0 || replaced != 0)
        printf("GLES%d table: %zu shared symbols kept from the other table, "
               "%zu overridden (live context is ES%d)\n",
               table_version, skipped, replaced, apkenv_active_gles_version());
    return register_hooks(filtered, n);
}

int register_hooks(const struct _hook *new_hooks, size_t count)
{
    if (hooks_count + count > HOOKS_MAX) {
        fprintf(stderr, "too many hooks (%d), increase HOOKS_MAX\n",
            hooks_count + count);
        return -1;
    }

    memcpy(&hooks[hooks_count], new_hooks, count * HOOK_SIZE);
    hooks_count += count;
    qsort(&hooks[0], hooks_count, HOOK_SIZE, hook_cmp);

    return 0;
}

void *get_builtin_lib_handle(const char *libname)
{
    size_t i;
    const char *base;

    if (libname == NULL)
        return NULL;

    base = strrchr(libname, '/');
    base = (base != NULL) ? base + 1 : libname;

    if (strcmp(base, "libGLESv1_CM.so") == 0) {
#ifdef APKENV_GLES
        if (!global.loader_seen_glesv1)
            register_hooks_gles(hooks_gles1, HOOKS_GLES1_COUNT, 1);
#endif
        global.loader_seen_glesv1 = 1;
    }
    else if (strcmp(base, "libGLESv2.so") == 0) {
#ifdef APKENV_GLES2
        if (!global.loader_seen_glesv2)
            register_hooks_gles(hooks_gles2, HOOKS_GLES2_COUNT, 2);
#endif
        global.loader_seen_glesv2 = 1;
    }

    /* Match on the BASENAME. Engines dlopen() the platform libs by absolute
     * Android path - Unity 3.5's libunity.so hardcodes "/system/lib/libEGL.so" -
     * and an exact-string compare misses those, so the builtin never answers and
     * the load fails. Temple Run 2 did this ~11 times per frame. */
    for (i = 0; i < sizeof(builtin_libs) / sizeof(builtin_libs[0]); i++) {
        if (strcmp(base, builtin_libs[i]) != 0)
            continue;
        /* Entries that are only builtin when bridged: if no host library has
         * taken over this SONAME, fall through so the apk's copy is loaded as
         * usual. This keeps every other port (WMW/PvZ/Alex) unaffected. */
        if (i == BUILTIN_LIB_MONO && !apkenv_hostlib_provides(base))
            return NULL;
        return &builtin_libs[i];
    }

    return NULL;
}

int is_builtin_lib_handle(void *handle)
{
    char *p = handle;
    return ((char *)builtin_libs <= p && p < (char *)builtin_libs + sizeof(builtin_libs));
}

int is_lib_optional(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(optional_libs) / sizeof(optional_libs[0]); i++)
        if (strcmp(name, optional_libs[i]) == 0)
            return i + 1;

    return 0;
}

/* --- call tracer (APKENV_TRACE_CALLS="lib:sym,lib:sym,...", max 16) ---------
 * Registers logging hooks for the named symbols; each forwards its first four
 * word arguments to the real symbol in the named library and logs entry/exit.
 * Brackets which engine->runtime call crashes, without a debugger. */
#define TRACE_MAX 16
static void *trace_fns[TRACE_MAX];
static char *trace_lib[TRACE_MAX], *trace_sym[TRACE_MAX];
static void *trace_real[TRACE_MAX];
static int trace_n = 0;
extern struct GlobalState global;
static void *trace_call(int i, int a, int b, int c, int d)
{
    if (!trace_real[i]) {
        char ln[128]; snprintf(ln, sizeof(ln), "%s%s", trace_lib[i], strstr(trace_lib[i], ".so") ? "" : ".so");
        /* A bridged host library first. This MUST come before the bionic
         * lookup: when a host library stands in for this SONAME the bionic copy
         * is not loaded at all, so apkenv_find_library() returns NULL and the
         * tracer would report "unresolved" and silently SWALLOW the call -
         * which looks exactly like the runtime hanging. */
        trace_real[i] = apkenv_hostlib_dlsym(ln, trace_sym[i]);
        if (!trace_real[i]) {   /* hook-free lookup straight in the library's symtab */
            soinfo *si = apkenv_find_library(ln);
            Elf32_Sym *sym = si ? apkenv_lookup_in_library(si, trace_sym[i]) : NULL;
            if (sym && sym->st_shndx != 0) trace_real[i] = (void *)(sym->st_value + si->base);
        }
        /* Never forward to ourselves: if the name resolved back to the tracer's
         * own wrapper we would recurse until the stack ran out. */
        if (trace_real[i] == (void *)trace_fns[i]) {
            fprintf(stderr, "[TRACE] %s resolved to the tracer wrapper - dropping\n", trace_sym[i]);
            trace_real[i] = NULL;
        }
    }
    fprintf(stderr, "[TRACE] -> %s(%#x, %#x, %#x, %#x) real=%p\n", trace_sym[i], a, b, c, d, trace_real[i]);
    if (!trace_real[i]) { fprintf(stderr, "[TRACE] %s unresolved\n", trace_sym[i]); return NULL; }
    void *r = ((void *(*)(int,int,int,int))trace_real[i])(a, b, c, d);
    fprintf(stderr, "[TRACE] <- %s = %p\n", trace_sym[i], r);
    return r;
}
#define TR(n) static void *trace_fn_##n(int a,int b,int c,int d){ return trace_call(n,a,b,c,d); }
TR(0) TR(1) TR(2) TR(3) TR(4) TR(5) TR(6) TR(7) TR(8) TR(9) TR(10) TR(11) TR(12) TR(13) TR(14) TR(15)
static void *trace_fns[TRACE_MAX] = { trace_fn_0,trace_fn_1,trace_fn_2,trace_fn_3,trace_fn_4,trace_fn_5,trace_fn_6,trace_fn_7,
    trace_fn_8,trace_fn_9,trace_fn_10,trace_fn_11,trace_fn_12,trace_fn_13,trace_fn_14,trace_fn_15 };
static void trace_register(void)
{
    const char *e = getenv("APKENV_TRACE_CALLS");
    if (!e || !e[0]) return;
    char *dup = strdup(e), *tok = strtok(dup, ",");
    int i;
    for (i = 0; i < HOOKS_MAX && hooks[i].name; i++) ;
    while (tok && trace_n < TRACE_MAX && i < HOOKS_MAX - 1) {
        char *colon = strchr(tok, ':');
        if (colon) { *colon = 0; trace_lib[trace_n] = strdup(tok); trace_sym[trace_n] = strdup(colon + 1); }
        else { trace_lib[trace_n] = strdup("libmono"); trace_sym[trace_n] = strdup(tok); }
        hooks[i].name = trace_sym[trace_n]; hooks[i].func = trace_fns[trace_n];
        fprintf(stderr, "[TRACE] hooking %s:%s\n", trace_lib[trace_n], trace_sym[trace_n]);
        i++; trace_n++; tok = strtok(NULL, ",");
    }
    hooks[i].name = NULL;
}

void hooks_init(void)
{
    trace_register();
    int i;

    for (i = 0; i < HOOKS_MAX; i++)
        if (hooks[i].name == NULL)
            break;
    hooks_count = i;

    /* Sort hooks so we can use binary search in apkenv_get_hooked_symbol() */
    qsort(&(hooks[0]), hooks_count, HOOK_SIZE, hook_cmp);
#ifdef APKENV_GLES
    qsort(hooks_gles1, HOOKS_GLES1_COUNT, HOOK_SIZE, hook_cmp);
#endif
#ifdef APKENV_GLES2
    qsort(hooks_gles2, HOOKS_GLES2_COUNT, HOOK_SIZE, hook_cmp);
#endif

    libc_wrappers_init();
}

static void no_hook(void)
{
    void *ra = __builtin_return_address(0);
    Dl_info di; memset(&di, 0, sizeof(di));
    extern int apkenv_dladdr_nolock; apkenv_dladdr_nolock = 1;
    int ok = apkenv_android_dladdr(ra, &di);
    fprintf(stderr, "called a function for which no hook is available (from %p%s%s +0x%x %s)\n", ra,
            ok ? " in " : "", ok ? di.dli_fname : "?", ok ? (unsigned)((char*)ra - (char*)di.dli_fbase) : 0,
            (ok && di.dli_sname) ? di.dli_sname : "");
    exit(6);
}

