
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

#define HOOKS_MAX 1024

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
};

static const char *builtin_libs[] = {
    [BUILTIN_LIB_EGL] = "libEGL.so",
    [BUILTIN_LIB_GLESV1] = "libGLESv1_CM.so",
    [BUILTIN_LIB_GLESV2] = "libGLESv2.so",
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
            return NULL;
        }
#endif
    }

    return apkenv_get_hooked_symbol(sym, 1);
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

    if (libname == NULL)
        return NULL;

    if (strcmp(libname, "libGLESv1_CM.so") == 0) {
#ifdef APKENV_GLES
        if (!global.loader_seen_glesv1)
            register_hooks(hooks_gles1, HOOKS_GLES1_COUNT);
#endif
        global.loader_seen_glesv1 = 1;
    }
    else if (strcmp(libname, "libGLESv2.so") == 0) {
#ifdef APKENV_GLES2
        if (!global.loader_seen_glesv2)
            register_hooks(hooks_gles2, HOOKS_GLES2_COUNT);
#endif
        global.loader_seen_glesv2 = 1;
    }

    for (i = 0; i < sizeof(builtin_libs) / sizeof(builtin_libs[0]); i++)
        if (strcmp(libname, builtin_libs[i]) == 0)
            return &builtin_libs[i];

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
static char *trace_lib[TRACE_MAX], *trace_sym[TRACE_MAX];
static void *trace_real[TRACE_MAX];
static int trace_n = 0;
extern struct GlobalState global;
static void *trace_call(int i, int a, int b, int c, int d)
{
    if (!trace_real[i]) {   /* hook-free lookup straight in the library's symtab */
        char ln[128]; snprintf(ln, sizeof(ln), "%s%s", trace_lib[i], strstr(trace_lib[i], ".so") ? "" : ".so");
        soinfo *si = apkenv_find_library(ln);
        Elf32_Sym *sym = si ? apkenv_lookup_in_library(si, trace_sym[i]) : NULL;
        if (sym && sym->st_shndx != 0) trace_real[i] = (void *)(sym->st_value + si->base);
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

