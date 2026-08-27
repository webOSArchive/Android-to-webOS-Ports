/*
 * webos_egl_shim.c — in-process interposition of the EGL calls Palm's SDL makes
 * while creating its GL context (plan/TEMPLERUN2-RENDER-INPUT.md, Stage G).
 *
 * apkenv is linked -rdynamic and -lEGL, so symbols defined HERE win symbol
 * resolution for libSDL's PLT calls (glibc searches the executable first).
 * SDL still owns the surface and the swap — we never call eglGetDisplay /
 * eglSwapBuffers ourselves, so the 3-layer-compositor rule in
 * webos://knowledge/pdk is respected.
 *
 * Two jobs:
 *   1. PASSIVE (always on): log exactly what SDL asks for and what EGL answers —
 *      attribute lists, every config returned (renderable type, surface type,
 *      RGBA/depth sizes, id), context attribs, and eglGetError() after a failure.
 *      This is the KB's "diff against something that works" method done in-process.
 *   2. ACTIVE (APKENV_EGL_FIX=<mode>, default 0 = off):
 *      1  filter the configs eglChooseConfig returned down to ones that carry
 *         EGL_OPENGL_ES2_BIT and EGL_WINDOW_BIT, when SDL asked for ES2.
 *      2  as 1, but also drop EGL_RENDERABLE_TYPE / EGL_SURFACE_TYPE / sizes from
 *         SDL's request, choose from the FULL config list ourselves (ES2 + window,
 *         prefer 8/8/8/8 + depth>=16), and hand SDL that.
 *      Pick the mode from the passive log; do not guess.
 */
#include <EGL/egl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHIM_MAX_ATTRS 64
#define SHIM_MAX_CFGS  64

typedef EGLBoolean (*choose_t)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext (*create_ctx_t)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface (*create_win_t)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
typedef EGLBoolean (*getattr_t)(EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef EGLint     (*geterr_t)(void);

static choose_t     real_choose;
static create_ctx_t real_create_ctx;
static create_win_t real_create_win;
static getattr_t    real_getattr;
static geterr_t     real_geterr;
static int fix_mode = -1;

static void
shim_init(void)
{
    if (fix_mode >= 0) return;
    const char *e = getenv("APKENV_EGL_FIX");
    fix_mode = e ? atoi(e) : 0;
    real_choose     = (choose_t)    dlsym(RTLD_NEXT, "eglChooseConfig");
    real_create_ctx = (create_ctx_t)dlsym(RTLD_NEXT, "eglCreateContext");
    real_create_win = (create_win_t)dlsym(RTLD_NEXT, "eglCreateWindowSurface");
    real_getattr    = (getattr_t)   dlsym(RTLD_NEXT, "eglGetConfigAttrib");
    real_geterr     = (geterr_t)    dlsym(RTLD_NEXT, "eglGetError");
    fprintf(stderr, "[EGLSHIM] active: fix_mode=%d real choose=%p ctx=%p win=%p attr=%p err=%p\n",
            fix_mode, (void*)real_choose, (void*)real_create_ctx, (void*)real_create_win,
            (void*)real_getattr, (void*)real_geterr);
}

static const char *
attr_name(EGLint a)
{
    switch (a) {
    case EGL_BUFFER_SIZE: return "BUFFER_SIZE";       case EGL_ALPHA_SIZE: return "ALPHA_SIZE";
    case EGL_BLUE_SIZE: return "BLUE_SIZE";           case EGL_GREEN_SIZE: return "GREEN_SIZE";
    case EGL_RED_SIZE: return "RED_SIZE";             case EGL_DEPTH_SIZE: return "DEPTH_SIZE";
    case EGL_STENCIL_SIZE: return "STENCIL_SIZE";     case EGL_CONFIG_ID: return "CONFIG_ID";
    case EGL_NATIVE_VISUAL_ID: return "NATIVE_VISUAL_ID";
    case EGL_NATIVE_RENDERABLE: return "NATIVE_RENDERABLE";
    case EGL_SAMPLES: return "SAMPLES";               case EGL_SAMPLE_BUFFERS: return "SAMPLE_BUFFERS";
    case EGL_SURFACE_TYPE: return "SURFACE_TYPE";     case EGL_RENDERABLE_TYPE: return "RENDERABLE_TYPE";
    case EGL_CONFORMANT: return "CONFORMANT";         case EGL_CONFIG_CAVEAT: return "CONFIG_CAVEAT";
    case EGL_CONTEXT_CLIENT_VERSION: return "CONTEXT_CLIENT_VERSION";
    case EGL_BIND_TO_TEXTURE_RGBA: return "BIND_TO_TEXTURE_RGBA";
    case EGL_LEVEL: return "LEVEL";                   case EGL_MAX_SWAP_INTERVAL: return "MAX_SWAP_INTERVAL";
    case EGL_MIN_SWAP_INTERVAL: return "MIN_SWAP_INTERVAL";
    case EGL_TRANSPARENT_TYPE: return "TRANSPARENT_TYPE";
    default: return NULL;
    }
}

static void
dump_attrs(const char *tag, const EGLint *a)
{
    char buf[1024]; int n = 0;
    if (!a) { fprintf(stderr, "[EGLSHIM] %s attribs=NULL\n", tag); return; }
    for (; a[0] != EGL_NONE && n < (int)sizeof(buf) - 64; a += 2) {
        const char *nm = attr_name(a[0]);
        if (nm) n += snprintf(buf + n, sizeof(buf) - n, " %s=0x%x", nm, a[1]);
        else    n += snprintf(buf + n, sizeof(buf) - n, " 0x%x=0x%x", a[0], a[1]);
    }
    fprintf(stderr, "[EGLSHIM] %s attribs:%s\n", tag, buf);
}

struct cfginfo { EGLint id, rt, st, r, g, b, a, d, s, caveat, samples; };

static void
query(EGLDisplay dpy, EGLConfig c, struct cfginfo *i)
{
    memset(i, 0, sizeof(*i));
    if (!real_getattr) return;
    real_getattr(dpy, c, EGL_CONFIG_ID, &i->id);
    real_getattr(dpy, c, EGL_RENDERABLE_TYPE, &i->rt);
    real_getattr(dpy, c, EGL_SURFACE_TYPE, &i->st);
    real_getattr(dpy, c, EGL_RED_SIZE, &i->r);   real_getattr(dpy, c, EGL_GREEN_SIZE, &i->g);
    real_getattr(dpy, c, EGL_BLUE_SIZE, &i->b);  real_getattr(dpy, c, EGL_ALPHA_SIZE, &i->a);
    real_getattr(dpy, c, EGL_DEPTH_SIZE, &i->d); real_getattr(dpy, c, EGL_STENCIL_SIZE, &i->s);
    real_getattr(dpy, c, EGL_CONFIG_CAVEAT, &i->caveat);
    real_getattr(dpy, c, EGL_SAMPLES, &i->samples);
}

static void
dump_cfg(const char *tag, EGLDisplay dpy, EGLConfig c)
{
    struct cfginfo i; query(dpy, c, &i);
    fprintf(stderr, "[EGLSHIM]   %s cfg=%p id=%d renderable=0x%x%s%s surface=0x%x%s "
            "rgba=%d/%d/%d/%d depth=%d stencil=%d samples=%d caveat=0x%x\n",
            tag, (void*)c, i.id, i.rt,
            (i.rt & EGL_OPENGL_ES_BIT) ? "[ES1]" : "", (i.rt & EGL_OPENGL_ES2_BIT) ? "[ES2]" : "",
            i.st, (i.st & EGL_WINDOW_BIT) ? "[WIN]" : "",
            i.r, i.g, i.b, i.a, i.d, i.s, i.samples, i.caveat);
}

static int
wants_es2(const EGLint *a)
{
    for (; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_RENDERABLE_TYPE && (a[1] & EGL_OPENGL_ES2_BIT)) return 1;
    return 0;
}

/* Score an ES2-capable window config: prefer 8/8/8/8, then depth>=16, then no caveat. */
static int
score(const struct cfginfo *i)
{
    int s = 0;
    if (!(i->rt & EGL_OPENGL_ES2_BIT) || !(i->st & EGL_WINDOW_BIT)) return -1;
    if (i->r == 8 && i->g == 8 && i->b == 8) s += 100;
    if (i->a == 8) s += 20;
    if (i->d >= 16) s += 50;
    if (i->d >= 24) s += 5;
    if (i->caveat == EGL_NONE) s += 10;
    if (i->samples == 0) s += 5;
    return s;
}

EGLBoolean
eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                EGLint config_size, EGLint *num_config)
{
    shim_init();
    dump_attrs("eglChooseConfig request", attrib_list);
    int es2 = wants_es2(attrib_list);

    if (fix_mode == 2 && es2 && configs && config_size > 0) {
        /* Ignore SDL's constraints; pick from the full list ourselves. */
        EGLConfig all[SHIM_MAX_CFGS]; EGLint n = 0;
        static const EGLint none[] = { EGL_NONE };
        EGLBoolean ok = real_choose(dpy, none, all, SHIM_MAX_CFGS, &n);
        fprintf(stderr, "[EGLSHIM] fix2: full config list ok=%d n=%d\n", ok, n);
        int best = -1, best_s = -1, k;
        for (k = 0; k < n; k++) {
            struct cfginfo i; query(dpy, all[k], &i);
            int sc = score(&i);
            dump_cfg(sc >= 0 ? "ES2-window" : "other", dpy, all[k]);
            if (sc > best_s) { best_s = sc; best = k; }
        }
        if (best >= 0) {
            configs[0] = all[best];
            if (num_config) *num_config = 1;
            dump_cfg("fix2 CHOSE", dpy, all[best]);
            return EGL_TRUE;
        }
        fprintf(stderr, "[EGLSHIM] fix2: no ES2 window config exists - passing through\n");
    }

    EGLBoolean ok = real_choose(dpy, attrib_list, configs, config_size, num_config);
    EGLint n = num_config ? *num_config : 0;
    fprintf(stderr, "[EGLSHIM] eglChooseConfig -> ok=%d num_config=%d (size %d)%s\n",
            ok, n, config_size, ok ? "" : " ERROR");
    if (!ok && real_geterr) fprintf(stderr, "[EGLSHIM]   eglGetError=0x%x\n", real_geterr());
    if (configs) {
        int k;
        for (k = 0; k < n && k < config_size; k++) dump_cfg("returned", dpy, configs[k]);
        if (fix_mode == 1 && es2 && n > 0) {
            int w = 0;
            for (k = 0; k < n && k < config_size; k++) {
                struct cfginfo i; query(dpy, configs[k], &i);
                if (score(&i) >= 0) configs[w++] = configs[k];
            }
            fprintf(stderr, "[EGLSHIM] fix1: kept %d of %d configs (ES2+window)\n", w, n);
            if (w > 0) { if (num_config) *num_config = w; }
            else fprintf(stderr, "[EGLSHIM] fix1: none qualify - leaving SDL's list alone\n");
        }
    }
    return ok;
}

EGLSurface
eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                       const EGLint *attrib_list)
{
    shim_init();
    dump_cfg("eglCreateWindowSurface with", dpy, config);
    dump_attrs("eglCreateWindowSurface", attrib_list);
    EGLSurface s = real_create_win(dpy, config, win, attrib_list);
    fprintf(stderr, "[EGLSHIM] eglCreateWindowSurface(win=%p) -> %p%s\n", (void*)win, (void*)s,
            s == EGL_NO_SURFACE ? " ERROR" : "");
    if (s == EGL_NO_SURFACE && real_geterr) fprintf(stderr, "[EGLSHIM]   eglGetError=0x%x\n", real_geterr());
    return s;
}

EGLContext
eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attrib_list)
{
    shim_init();
    dump_cfg("eglCreateContext with", dpy, config);
    dump_attrs("eglCreateContext", attrib_list);
    EGLContext c = real_create_ctx(dpy, config, share, attrib_list);
    fprintf(stderr, "[EGLSHIM] eglCreateContext -> %p%s\n", (void*)c, c == EGL_NO_CONTEXT ? " ERROR" : "");
    if (c == EGL_NO_CONTEXT && real_geterr) {
        EGLint e = real_geterr();
        fprintf(stderr, "[EGLSHIM]   eglGetError=0x%x (%s)\n", e,
                e == EGL_BAD_ALLOC ? "BAD_ALLOC" : e == EGL_BAD_CONFIG ? "BAD_CONFIG" :
                e == EGL_BAD_ATTRIBUTE ? "BAD_ATTRIBUTE" : e == EGL_BAD_MATCH ? "BAD_MATCH" : "?");
    }
    return c;
}

/* ---- diagnostic probe -----------------------------------------------------
 * Can THIS process, in its current state, create an ES2 context at all?
 * tools/egl2test.c answers that from a novacom shell (27/27 configs OK, raw,
 * +PDL, and with SDL's ES1 context current). Running the same probe from
 * inside apkenv - and from inside the app JAIL when launched from the icon -
 * is what tells us whether the jail is the difference.
 * Gated on APKENV_EGL_PROBE=1 so it costs nothing in normal runs. */
void
apkenv_egl_probe(const char *tag)
{
    shim_init();
    if (!getenv("APKENV_EGL_PROBE")) return;

    EGLDisplay dpy = eglGetCurrentDisplay();
    const char *how = "current";
    if (dpy == EGL_NO_DISPLAY) {
        EGLint maj = 0, min = 0;
        how = "fresh";
        dpy = eglGetDisplay((EGLNativeDisplayType)0);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
            dpy = eglGetDisplay((EGLNativeDisplayType)1);
            if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
                fprintf(stderr, "[EGLPROBE %s] no usable display (err=0x%x)\n",
                        tag, real_geterr ? real_geterr() : 0);
                return;
            }
        }
    }

    EGLConfig cfgs[SHIM_MAX_CFGS]; EGLint n = 0, k;
    if (!eglGetConfigs(dpy, cfgs, SHIM_MAX_CFGS, &n)) {
        fprintf(stderr, "[EGLPROBE %s] eglGetConfigs failed err=0x%x\n",
                tag, real_geterr ? real_geterr() : 0);
        return;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint a1[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
    static const EGLint a2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    int es1_ok = 0, es2_ok = 0, es2_cap = 0; EGLint first_err = 0, first_id = 0;

    for (k = 0; k < n; k++) {
        struct cfginfo i; query(dpy, cfgs[k], &i);
        if (i.rt & EGL_OPENGL_ES_BIT) {
            EGLContext c = real_create_ctx(dpy, cfgs[k], EGL_NO_CONTEXT, a1);
            if (c != EGL_NO_CONTEXT) { es1_ok++; eglDestroyContext(dpy, c); }
        }
        if (i.rt & EGL_OPENGL_ES2_BIT) {
            es2_cap++;
            EGLContext c = real_create_ctx(dpy, cfgs[k], EGL_NO_CONTEXT, a2);
            if (c != EGL_NO_CONTEXT) { es2_ok++; eglDestroyContext(dpy, c); }
            else if (!first_err) {
                first_err = real_geterr ? real_geterr() : 0;
                first_id = i.id;
            }
        }
    }
    fprintf(stderr, "[EGLPROBE %s] display=%p (%s) configs=%d  ES1 ctx OK %d  "
            "ES2-capable %d  ES2 ctx OK %d%s",
            tag, (void*)dpy, how, n, es1_ok, es2_cap, es2_ok, es2_ok ? "\n" : "");
    if (!es2_ok)
        fprintf(stderr, "  first failure: cfg id=%d err=0x%x (%s)\n", first_id, first_err,
                first_err == EGL_BAD_ALLOC ? "BAD_ALLOC" :
                first_err == EGL_BAD_CONFIG ? "BAD_CONFIG" :
                first_err == EGL_BAD_ATTRIBUTE ? "BAD_ATTRIBUTE" : "?");
}

/* ---- ES2 warm-up ----------------------------------------------------------
 * THE fix for "Could not create EGL context" on the TouchPad.
 *
 * Measured, not theorised (plan/logs/tr2-es2-1.log): SDL's eglCreateContext
 * with EGL_CONTEXT_CLIENT_VERSION=2 returns EGL_BAD_ALLOC when it is the FIRST
 * EGL work the process does - yet tools/egl2test.c creates ES2 contexts on all
 * 27 configs from a bare process, with PDL, and even with SDL's ES1 context
 * current. The difference is that egl2test opens and initialises a display and
 * makes a context first. Doing the same here before SDL asks makes SDL's own
 * ES2 request succeed (verified on device: gles_version=2).
 *
 * This does NOT create a window surface and does NOT call eglSwapBuffers, so
 * SDL still owns the surface and the 3-layer compositor rule in
 * webos://knowledge/pdk still holds.
 *
 * Set APKENV_EGL_WARMUP=0 to skip it (e.g. to reproduce the old behaviour). */
void
apkenv_egl_warmup(void)
{
    const char *off = getenv("APKENV_EGL_WARMUP");
    if (off != NULL && off[0] == '0') {
        fprintf(stderr, "[EGLWARM] disabled by APKENV_EGL_WARMUP=0\n");
        return;
    }
    shim_init();

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)0);
    EGLint maj = 0, min = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        dpy = eglGetDisplay((EGLNativeDisplayType)1);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
            fprintf(stderr, "[EGLWARM] no usable display; skipping warm-up\n");
            return;
        }
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLConfig cfgs[SHIM_MAX_CFGS]; EGLint n = 0, k;
    if (!eglGetConfigs(dpy, cfgs, SHIM_MAX_CFGS, &n) || n <= 0) {
        fprintf(stderr, "[EGLWARM] eglGetConfigs failed; skipping warm-up\n");
        return;
    }

    static const EGLint a1[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
    static const EGLint a2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    int made = 0;
    for (k = 0; k < n && !made; k++) {
        struct cfginfo i; query(dpy, cfgs[k], &i);
        if (!(i.rt & EGL_OPENGL_ES2_BIT) || !(i.st & EGL_WINDOW_BIT))
            continue;

        /* An ES1 context FIRST, then ES2. Measured: creating ES2 as the very
         * first context in the process returns EGL_BAD_ALLOC on every one of
         * the 27 configs, but after one ES1 context has been created (and even
         * destroyed) ES2 succeeds on all of them - which is why
         * tools/egl2test.c, which tries ES1 before ES2 per config, saw 27/27.
         * The Adreno driver evidently defers some per-process initialisation
         * to the first context and only the ES1 path performs it. */
        EGLContext c1 = real_create_ctx(dpy, cfgs[k], EGL_NO_CONTEXT, a1);
        if (c1 != EGL_NO_CONTEXT)
            eglDestroyContext(dpy, c1);
        else
            fprintf(stderr, "[EGLWARM] ES1 priming context failed on cfg id=%d (err=0x%x)\n",
                    i.id, real_geterr ? real_geterr() : 0);

        EGLContext c = real_create_ctx(dpy, cfgs[k], EGL_NO_CONTEXT, a2);
        if (c != EGL_NO_CONTEXT) {
            eglDestroyContext(dpy, c);
            made = 1;
            fprintf(stderr, "[EGLWARM] EGL %d.%d up, ES1-primed, ES2 context proven on "
                            "cfg id=%d (%d configs) - SDL's ES2 request should now succeed\n",
                    maj, min, i.id, n);
        }
    }
    if (!made)
        fprintf(stderr, "[EGLWARM] no ES2 context could be created on any of %d configs "
                        "(err=0x%x) - expect the ES1 fallback\n",
                n, real_geterr ? real_geterr() : 0);
    /* Deliberately NOT eglTerminate(): the display must stay initialised. */
}
