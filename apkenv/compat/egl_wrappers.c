#include "egl_wrappers.h"
#include "hooks.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

#ifdef APKENV_DEBUG
#  define WRAPPERS_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#  define WRAPPERS_DEBUG_PRINTF(...)
#endif

void *
my_eglGetProcAddress(const char *procname)
{
    WRAPPERS_DEBUG_PRINTF("eglGetProcAddress(%s)\n", procname);
    void *sym = apkenv_get_hooked_symbol(procname, 1);
    /* libunity imports eglGetProcAddress and almost nothing else from EGL, so
     * this is the engine's whole conversation with the GL loader - including
     * whatever it probes to decide which renderer to build. Log it. */
    /* Bounded, both lines: Unity 4 re-asks for eglGetSystemTime(Frequency)NV on
     * every frame when the answer is NULL - 680 unbounded "unimplemented" lines
     * in Aralon's first seconds, burying the crash dump that mattered. */
    {
        static int n;
        if (n < 200) { n++;
            printf("[EGLPROC] %s -> %s\n", procname, sym ? "ok" : "NULL");
            if (sym == NULL)
                printf("eglGetProcAddress: unimplemented: %s\n", procname);
        }
    }
    return sym;
}

EGLDisplay
my_eglGetDisplay(EGLNativeDisplayType display_id)
{
    WRAPPERS_DEBUG_PRINTF("eglGetDisplay(%x)\n", (int)display_id);
    return (void *)0xc00fa15e;
}

extern struct GlobalState global;

/* ---- Real shared contexts for engine GL threads (opt-in) --------------------
 * Every EGL entry point below is a stub: the platform (SDL/PDL) owns the one
 * real context, on the main thread. That is right for single-threaded engines
 * and silently wrong for one that streams textures from a second thread, as
 * Android's GLSurfaceView + a shared background context allows: that thread's
 * GL calls have no current context and do nothing. The tell is glGetError()
 * == GL_NO_ERROR while a glGetIntegerv never writes its output. (Cut the Rope
 * HD, another developer's port; likely also WMW2's multi-threaded GL stall.)
 *
 * apkenv_egl_shared_contexts_enable() makes the engine's eglCreateContext
 * create a REAL context sharing the platform context's object namespace, and
 * eglMakeCurrent on a non-main thread bind it over a 4x4 pbuffer (surfaceless
 * if no pbuffer config exists). On the main thread eglMakeCurrent stays a
 * stub, so the platform's context is never displaced. For engines that do GL
 * on a worker without calling EGL (the Java host made it current),
 * apkenv_egl_ensure_thread_context() binds one on the calling thread.
 * Off by default: shipped ports keep the stubs. */
static int egl_shared_on = 0;
static EGLDisplay egl_main_dpy = EGL_NO_DISPLAY;
static EGLConfig egl_main_cfg = NULL;
static EGLContext egl_main_ctx = EGL_NO_CONTEXT;
static EGLint egl_main_version = 1;

#define EGL_SHARED_MAX 16
static struct { EGLContext ctx; EGLSurface pb; } egl_shared[EGL_SHARED_MAX];
static pthread_mutex_t egl_shared_lock = PTHREAD_MUTEX_INITIALIZER;

static int
egl_on_main_thread(void)
{
    return pthread_equal(pthread_self(), global.gl_thread_id);
}

/* Record the platform's display/config/context. Must run on the main thread
 * with that context current; apkenv.c calls it right after platform init. */
void
apkenv_egl_capture_main_context(void)
{
    if (!egl_shared_on || egl_main_ctx != EGL_NO_CONTEXT || !egl_on_main_thread()) {
        return;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[EGLSHARE] no current platform context to share (dpy=%p ctx=%p)\n",
                dpy, ctx);
        return;
    }

    EGLint id = -1, n = 0, i;
    eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &id);
    eglQueryContext(dpy, ctx, EGL_CONTEXT_CLIENT_VERSION, &egl_main_version);

    /* eglGetConfigs, not eglChooseConfig: the webOS EGL shim filters the latter. */
    EGLConfig cfgs[64];
    if (eglGetConfigs(dpy, cfgs, 64, &n)) {
        for (i = 0; i < n; i++) {
            EGLint cid = -2;
            if (eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_ID, &cid) && cid == id) {
                egl_main_cfg = cfgs[i];
                break;
            }
        }
    }
    if (egl_main_cfg == NULL) {
        fprintf(stderr, "[EGLSHARE] config id %d of the platform context not found\n", id);
        return;
    }

    egl_main_dpy = dpy;
    egl_main_ctx = ctx;
    fprintf(stderr, "[EGLSHARE] platform context %p (dpy %p, config id %d, ES%d) captured\n",
            ctx, dpy, id, egl_main_version);
}

void
apkenv_egl_shared_contexts_enable(void)
{
    egl_shared_on = 1;
    fprintf(stderr, "[EGLSHARE] real shared contexts for engine GL threads enabled\n");
    apkenv_egl_capture_main_context();
}

static EGLSurface
egl_make_pbuffer(void)
{
    static const EGLint pb_attrs[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
    EGLSurface pb = eglCreatePbufferSurface(egl_main_dpy, egl_main_cfg, pb_attrs);
    if (pb != EGL_NO_SURFACE) {
        return pb;
    }

    /* The window config may lack EGL_PBUFFER_BIT; any pbuffer config with the
     * same client API will do - the context, not the surface, is what shares. */
    EGLint want = (egl_main_version >= 2) ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT;
    EGLConfig cfgs[64];
    EGLint n = 0, i;
    if (eglGetConfigs(egl_main_dpy, cfgs, 64, &n)) {
        for (i = 0; i < n; i++) {
            EGLint st = 0, rt = 0;
            eglGetConfigAttrib(egl_main_dpy, cfgs[i], EGL_SURFACE_TYPE, &st);
            eglGetConfigAttrib(egl_main_dpy, cfgs[i], EGL_RENDERABLE_TYPE, &rt);
            if ((st & EGL_PBUFFER_BIT) && (rt & want)) {
                pb = eglCreatePbufferSurface(egl_main_dpy, cfgs[i], pb_attrs);
                if (pb != EGL_NO_SURFACE) {
                    return pb;
                }
            }
        }
    }
    return EGL_NO_SURFACE;
}

static int
egl_shared_find(EGLContext ctx)
{
    int i;
    for (i = 0; i < EGL_SHARED_MAX; i++) {
        if (egl_shared[i].ctx == ctx && ctx != EGL_NO_CONTEXT) {
            return i;
        }
    }
    return -1;
}

/* A new real context sharing the platform's, registered; EGL_NO_CONTEXT on failure. */
static EGLContext
egl_shared_create(void)
{
    if (egl_main_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[EGLSHARE] no platform context captured; cannot share\n");
        return EGL_NO_CONTEXT;
    }

    EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, egl_main_version, EGL_NONE };
    EGLContext ctx = eglCreateContext(egl_main_dpy, egl_main_cfg, egl_main_ctx, attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[EGLSHARE] eglCreateContext(shared) failed: 0x%x\n", eglGetError());
        return EGL_NO_CONTEXT;
    }

    pthread_mutex_lock(&egl_shared_lock);
    int i;
    for (i = 0; i < EGL_SHARED_MAX && egl_shared[i].ctx != EGL_NO_CONTEXT; i++);
    if (i < EGL_SHARED_MAX) {
        egl_shared[i].ctx = ctx;
        egl_shared[i].pb = EGL_NO_SURFACE;
    }
    pthread_mutex_unlock(&egl_shared_lock);

    if (i == EGL_SHARED_MAX) {
        fprintf(stderr, "[EGLSHARE] too many shared contexts (%d)\n", EGL_SHARED_MAX);
        eglDestroyContext(egl_main_dpy, ctx);
        return EGL_NO_CONTEXT;
    }

    fprintf(stderr, "[EGLSHARE] created shared context %p\n", ctx);
    return ctx;
}

static EGLBoolean
egl_shared_bind(EGLContext ctx)
{
    pthread_mutex_lock(&egl_shared_lock);
    int i = egl_shared_find(ctx);
    EGLSurface pb = EGL_NO_SURFACE;
    if (i >= 0) {
        if (egl_shared[i].pb == EGL_NO_SURFACE) {
            egl_shared[i].pb = egl_make_pbuffer();
        }
        pb = egl_shared[i].pb;
    }
    pthread_mutex_unlock(&egl_shared_lock);

    if (i < 0) {
        return EGL_FALSE;
    }

    /* EGL_NO_SURFACE is surfaceless; it works only where the driver allows it. */
    EGLBoolean ok = eglMakeCurrent(egl_main_dpy, pb, pb, ctx);
    fprintf(stderr, "[EGLSHARE] thread %lx: bind context %p over %s -> %s (0x%x)\n",
            (unsigned long)pthread_self(), ctx, pb == EGL_NO_SURFACE ? "no surface" : "pbuffer",
            ok ? "ok" : "FAILED", ok ? 0 : eglGetError());
    return ok;
}

/* Engine threads that get a context at start (opt-in): those whose start
 * routine lives in this library. Naming the library keeps it off threads that
 * never touch GL - FMOD's mixer threads run on small stacks. */
static char *egl_thread_lib = NULL;

void
apkenv_egl_engine_thread_contexts_enable(const char *libname)
{
    apkenv_egl_shared_contexts_enable();
    egl_thread_lib = strdup(libname);
    fprintf(stderr, "[EGLSHARE] threads started from %s get a shared context\n", libname);
}

int
apkenv_egl_thread_wants_context(void *start_routine)
{
    if (egl_thread_lib == NULL) {
        return 0;
    }
    Dl_info di;
    memset(&di, 0, sizeof(di));
    if (!apkenv_android_dladdr(start_routine, &di) || di.dli_fname == NULL) {
        return 0;
    }
    return strstr(di.dli_fname, egl_thread_lib) != NULL;
}

void
apkenv_egl_release_thread_context(void)
{
    EGLContext ctx = eglGetCurrentContext();
    if (egl_shared_find(ctx) >= 0) {
        my_eglDestroyContext(egl_main_dpy, ctx);
    }
}

int
apkenv_egl_ensure_thread_context(void)
{
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        return 1;
    }
    if (!egl_shared_on || egl_on_main_thread()) {
        return 0;
    }
    EGLContext ctx = egl_shared_create();
    return ctx != EGL_NO_CONTEXT && egl_shared_bind(ctx);
}

EGLBoolean
my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    fprintf(stderr, "STUB eglSwapBuffers(dpy=%p, surface=%p\n", dpy, surface);

    global.platform->update();

    return EGL_FALSE;
}

EGLBoolean
my_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    fprintf(stderr, "STUB eglInitialize(dpy=%p, major=%p, minor=%p)\n", dpy, major, minor);

    return EGL_TRUE;
}

EGLBoolean
my_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config)
{
    fprintf(stderr, "STUB eglChooseConfig(dpy=%p, attrib_list=%p, configs=%p, config_size=%d, num_config=%p)\n",
            dpy, attrib_list, configs, config_size, num_config);

    *num_config = 1;

    return EGL_TRUE;
}

EGLBoolean
my_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    fprintf(stderr, "STUB eglGetConfigAttrib(dpy=%p, config=%p, attribute=%x, value=%p)\n",
            dpy, config, attribute, value);

    switch (attribute) {
        case EGL_RED_SIZE:
        case EGL_GREEN_SIZE:
        case EGL_BLUE_SIZE:
            *value = 8;
            return EGL_TRUE;
        case EGL_DEPTH_SIZE:
            *value = 16;
            return EGL_TRUE;
        case 0x302e /* EGL_NATIVE_VISUAL_ID */:
            *value = 0xF00D;
            return EGL_TRUE;
        default:
            break;
    }

    fprintf(stderr, "Unhandled EGL attribute: %x\n", attribute);
    return EGL_FALSE;
}

EGLSurface
my_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list)
{
    fprintf(stderr, "STUB eglCreateWindowSurface(dpy=%p, config=%p, win=%ld, attrib_list=%p)\n",
            dpy, config, win, attrib_list);

    return (EGLSurface)0xCAFEBABE;
}

EGLContext
my_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list)
{
    fprintf(stderr, "STUB eglCreateContext(dpy=%p, config=%p, share_context=%p, attrib_list=%p)\n",
            dpy, config, share_context, attrib_list);

    if (egl_shared_on) {
        EGLContext ctx = egl_shared_create();
        if (ctx != EGL_NO_CONTEXT) {
            return ctx;
        }
    }

    return (EGLContext)0xF00DFACE;
}

EGLBoolean
my_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    fprintf(stderr, "STUB eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p)\n", dpy, draw, read, ctx);

    /* The main thread keeps the platform's context: stay a stub there. */
    if (egl_shared_on && !egl_on_main_thread()) {
        if (ctx == EGL_NO_CONTEXT) {
            if (egl_shared_find(eglGetCurrentContext()) >= 0) {
                return eglMakeCurrent(egl_main_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);
            }
            return EGL_TRUE;
        }
        if (egl_shared_find(ctx) >= 0) {
            return egl_shared_bind(ctx);
        }
        /* A context we did not make (the stub's 0xF00DFACE, or one the Java
         * host's EGL10 handed out): give this thread a shared one of its own. */
        return apkenv_egl_ensure_thread_context() ? EGL_TRUE : EGL_FALSE;
    }

    return EGL_TRUE;
}

EGLBoolean
my_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)
{
    fprintf(stderr, "STUB eglQuerySurface(dpy=%p, surface=%p, attribute=%x, value=%p\n", dpy, surface, attribute, value);

    int width, height;
    global.platform->get_size(&width, &height);

    switch (attribute) {
        case EGL_WIDTH:
            *value = width;
            return EGL_TRUE;
        case EGL_HEIGHT:
            *value = height;
            return EGL_TRUE;
        default:
            break;
    }

    fprintf(stderr, "Unhandled attribute: %x\n", attribute);
    return EGL_FALSE;
}

EGLBoolean
my_eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    fprintf(stderr, "STUB eglDestroyContext(dpy=%p, ctx=%p)\n", dpy, ctx);

    if (egl_shared_on) {
        pthread_mutex_lock(&egl_shared_lock);
        int i = egl_shared_find(ctx);
        EGLSurface pb = EGL_NO_SURFACE;
        if (i >= 0) {
            pb = egl_shared[i].pb;
            egl_shared[i].ctx = EGL_NO_CONTEXT;
            egl_shared[i].pb = EGL_NO_SURFACE;
        }
        pthread_mutex_unlock(&egl_shared_lock);
        if (i >= 0) {
            if (eglGetCurrentContext() == ctx) {
                eglMakeCurrent(egl_main_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            if (pb != EGL_NO_SURFACE) {
                eglDestroySurface(egl_main_dpy, pb);
            }
            /* EGL defers the delete if another thread still has it current. */
            return eglDestroyContext(egl_main_dpy, ctx);
        }
    }

    return EGL_TRUE;
}

EGLBoolean
my_eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    fprintf(stderr, "STUB eglDestroySurface(dpy=%p, surface=%p)\n", dpy, surface);

    return EGL_TRUE;
}

EGLBoolean
my_eglTerminate(EGLDisplay dpy)
{
    fprintf(stderr, "STUB eglTerminate(dpy=%p)\n", dpy);

    return EGL_TRUE;
}
