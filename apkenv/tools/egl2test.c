/*
 * egl2test.c - can this TouchPad create an OpenGL ES 2.0 context at all, and
 * if so, under what process state?  (plan/TEMPLERUN2-RENDER-INPUT.md, Stage G2)
 *
 * The in-process EGL shim proved the CONFIG is not the problem: SDL is handed
 * config id 5, which advertises [ES1][ES2], WINDOW, rgba 8/8/8/8, depth 16,
 * caveat EGL_NONE - and eglCreateContext(CLIENT_VERSION=2) still returns
 * EGL_BAD_ALLOC.  So the question is no longer "which config" but "is an ES2
 * context obtainable in this process at all".
 *
 * Three modes isolate the three candidate culprits.  Run all three:
 *   raw   eglGetDisplay/eglInitialize only.  No PDL, no SDL.  If ES2 fails
 *         HERE, the driver/kernel refuses ES2 to this process outright and
 *         nothing in apkenv can fix it.
 *   pdl   PDL_Init(0) first, then the same.  Isolates PDL/compositor setup.
 *   sdl   PDL_Init + SDL_Init + SDL_SetVideoMode(ES1) - exactly what
 *         platform/webos.c does - then try an ES2 context on the SAME display.
 *         Isolates "SDL already owns an ES1 context/surface".
 *
 * In every mode it enumerates ALL configs, prints their attributes, and for
 * each config tries an ES1 context and an ES2 context, reporting eglGetError()
 * for each failure.  The summary line is the answer.
 *
 * Build+run:  tools/egl2-device-run.sh
 */
#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PDL.h>

#define MAXCFG 64

static const char *
errstr(EGLint e)
{
    switch (e) {
    case EGL_SUCCESS: return "SUCCESS";
    case EGL_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "BAD_ACCESS";
    case EGL_BAD_ALLOC: return "BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE: return "BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "BAD_DISPLAY";
    case EGL_BAD_MATCH: return "BAD_MATCH";
    case EGL_BAD_NATIVE_WINDOW: return "BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "BAD_SURFACE";
    default: return "?";
    }
}

static int
probe(EGLDisplay dpy, const char *tag)
{
    EGLConfig cfgs[MAXCFG];
    EGLint n = 0, k;
    int es2_ok = 0, es2_try = 0, es1_ok = 0;

    if (!eglGetConfigs(dpy, cfgs, MAXCFG, &n)) {
        printf("[%s] eglGetConfigs FAILED: %s\n", tag, errstr(eglGetError()));
        return 0;
    }
    printf("[%s] %d configs\n", tag, n);

    /* eglBindAPI is ES-by-default in EGL 1.4, but say it explicitly: a wrong
     * bound API is a documented source of BAD_ALLOC/BAD_MATCH here. */
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        printf("[%s] eglBindAPI(ES) failed: %s\n", tag, errstr(eglGetError()));

    for (k = 0; k < n; k++) {
        EGLint id = 0, rt = 0, st = 0, r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, cav = 0;
        eglGetConfigAttrib(dpy, cfgs[k], EGL_CONFIG_ID, &id);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_RENDERABLE_TYPE, &rt);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_SURFACE_TYPE, &st);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_DEPTH_SIZE, &d);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_STENCIL_SIZE, &s);
        eglGetConfigAttrib(dpy, cfgs[k], EGL_CONFIG_CAVEAT, &cav);

        char v1[64] = "-", v2[64] = "-";
        static const EGLint a1[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
        static const EGLint a2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

        if (rt & EGL_OPENGL_ES_BIT) {
            EGLContext c = eglCreateContext(dpy, cfgs[k], EGL_NO_CONTEXT, a1);
            if (c != EGL_NO_CONTEXT) { snprintf(v1, sizeof v1, "OK"); es1_ok++; eglDestroyContext(dpy, c); }
            else snprintf(v1, sizeof v1, "FAIL:%s", errstr(eglGetError()));
        }
        if (rt & EGL_OPENGL_ES2_BIT) {
            es2_try++;
            EGLContext c = eglCreateContext(dpy, cfgs[k], EGL_NO_CONTEXT, a2);
            if (c != EGL_NO_CONTEXT) { snprintf(v2, sizeof v2, "OK"); es2_ok++; eglDestroyContext(dpy, c); }
            else snprintf(v2, sizeof v2, "FAIL:%s", errstr(eglGetError()));
        }
        printf("[%s] cfg id=%-3d rend=0x%x%s%s surf=0x%x%s rgba=%d/%d/%d/%d d=%d s=%d cav=0x%x "
               "ctxES1=%s ctxES2=%s\n",
               tag, id, rt, (rt & EGL_OPENGL_ES_BIT) ? "[ES1]" : "",
               (rt & EGL_OPENGL_ES2_BIT) ? "[ES2]" : "", st,
               (st & EGL_WINDOW_BIT) ? "[WIN]" : "", r, g, b, a, d, s, cav, v1, v2);
    }
    printf("[%s] SUMMARY: ES1 contexts OK %d, ES2-capable configs %d, ES2 contexts OK %d\n",
           tag, es1_ok, es2_try, es2_ok);
    return es2_ok;
}

static EGLDisplay
open_display(const char *tag)
{
    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)0);
    EGLint maj = 0, min = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        printf("[%s] display 0 unusable (%s); trying display 1\n", tag, errstr(eglGetError()));
        dpy = eglGetDisplay((EGLNativeDisplayType)1);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
            printf("[%s] no usable EGLDisplay: %s\n", tag, errstr(eglGetError()));
            return EGL_NO_DISPLAY;
        }
    }
    printf("[%s] EGL %d.%d vendor=%s version=%s\n", tag, maj, min,
           eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    printf("[%s] EGL extensions: %s\n", tag, eglQueryString(dpy, EGL_EXTENSIONS));
    return dpy;
}

int
main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "raw";
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== egl2test mode=%s ===\n", mode);

    if (!strcmp(mode, "pdl") || !strcmp(mode, "sdl")) {
        printf("PDL_Init(0)...\n");
        PDL_Init(0);
        PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES);
        PDL_GesturesEnable(PDL_FALSE);
    }

    if (!strcmp(mode, "sdl")) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            printf("SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        /* Exactly platform/webos.c's ES1 path, which is known to work. */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_Surface *scr = SDL_SetVideoMode(0, 0, 0, SDL_OPENGLES | SDL_FULLSCREEN);
        if (!scr) { printf("SDL_SetVideoMode(ES1) failed: %s\n", SDL_GetError()); return 1; }
        printf("SDL ES1 surface %dx%d up; EGL display now owned by SDL\n", scr->w, scr->h);

        EGLDisplay cur = eglGetCurrentDisplay();
        printf("eglGetCurrentDisplay=%p ctx=%p surf=%p\n", (void*)cur,
               (void*)eglGetCurrentContext(), (void*)eglGetCurrentSurface(EGL_DRAW));
        if (cur != EGL_NO_DISPLAY) {
            probe(cur, "sdl-current");
            /* Also: does releasing SDL's context free whatever ES2 needs? */
            printf("--- releasing SDL's current context, retrying ---\n");
            eglMakeCurrent(cur, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            probe(cur, "sdl-released");
            return 0;
        }
        printf("SDL kept no current display; falling through to a fresh one\n");
    }

    EGLDisplay dpy = open_display(mode);
    if (dpy == EGL_NO_DISPLAY) return 1;
    probe(dpy, mode);
    return 0;
}
