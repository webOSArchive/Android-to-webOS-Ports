/**
 * apkenv — webOS (HP TouchPad) platform backend
 *
 * Reconstructed for the slim Android-NDK -> webOS game wrapper.
 * Based on platform/harmattan.c, with X11/MeeGo removed and PDL (Palm Device
 * Library) added. See android-port-shim.md and the BUILD-STATE.md notes.
 *
 * Hard-won rules baked in here (do not "simplify" away):
 *   - PDL_Init() MUST be called BEFORE SDL_Init(). The TouchPad has a 3-layer
 *     display compositor; wrong init order yields malformed touch events
 *     (inconsistent finger ids across down/move/up).
 *   - PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES): the default
 *     LESSTOUCHES garbles multi-finger streams (needed for swipe/carve).
 *   - PDL_GesturesEnable(PDL_FALSE): stop the system eating screen-edge touches.
 *   - Keep the NATIVE landscape framebuffer (SDL_SetVideoMode(0,0,0,...)); the
 *     PDK framebuffer is hardwired landscape 1024x768. Portrait rotation is done
 *     by the per-game module via apkenv's GLES1 fixed-function hooks, NOT here.
 *   - Never use EGL directly; SDL owns the GL context (SDL_GL_SwapBuffers).
 **/

#include "../apkenv.h"
#include "../compat/hooks.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>

#include <SDL.h>
#include <PDL.h>

#include "common/sdl_accelerometer_impl.h"
#include "common/sdl_audio_impl.h"
#include "common/sdl_mixer_impl.h"

#include "common/input_transform.h"

/* platform/webos_egl_shim.c: ES1/ES2 context probe, gated on APKENV_EGL_PROBE */
void apkenv_egl_probe(const char *tag);
void apkenv_egl_warmup(void);
void apkenv_gles1_bind_driver(int gles_version);

struct PlatformPriv {
    SDL_Surface *screen;
};

static struct PlatformPriv priv;

/* Per-finger tracking. SDL 1.2 on webOS carries the finger index (0..4) in the
 * non-standard event.button.which / event.motion.which field. SDL emits MOTION
 * events even when no finger is down for that index, so we gate MOVE on a
 * known-down finger. The module aggregates these into multi-touch arrays. */
#define WEBOS_MAX_FINGERS 5
static int finger_down[WEBOS_MAX_FINGERS];

static int
webos_init(int gles_version)
{
    apkenv_egl_probe("pre-PDL");
    apkenv_egl_warmup();

    /* PDL FIRST — before SDL. This is the 3-layer-compositor requirement. */
    PDL_Init(0);
    PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES);
    PDL_GesturesEnable(PDL_FALSE);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "webos_init: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    apkenv_egl_probe("post-PDL/SDL_Init");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gles_version);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* 0,0,0 -> keep the device's native landscape mode (1024x768). */
    priv.screen = SDL_SetVideoMode(0, 0, 0, SDL_OPENGLES | SDL_FULLSCREEN);

    if (priv.screen == NULL && gles_version != 1) {
        /* Asking this SDL for an ES2 context ALWAYS fails on the TouchPad: it
         * requests EGL_CONTEXT_CLIENT_VERSION=2 / EGL_RENDERABLE_TYPE=ES2_BIT
         * and the Adreno 220 driver answers EGL_BAD_ALLOC, for every config and
         * size (webos://knowledge/opengl-es-on-touchpad - measured, not
         * theorised: nine pixel formats fail identically, while raw EGL from a
         * shell succeeds on all 27 configs). The device reports
         * GL_VERSION "OpenGL ES-CM 1.1" through this path.
         *
         * Degrade instead of dying: an engine that would merely look wrong under
         * fixed-function is strictly better than one that will not start. */
        fprintf(stderr, "webos_init: GLESv%d context refused (%s); "
                        "falling back to GLESv1\n", gles_version, SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        priv.screen = SDL_SetVideoMode(0, 0, 0, SDL_OPENGLES | SDL_FULLSCREEN);
        gles_version = 1;
    }

    if (priv.screen == NULL) {
        fprintf(stderr, "webos_init: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_ShowCursor(0);

    memset(finger_down, 0, sizeof(finger_down));

    apkenv_accelerometer_register(sdl_accelerometer);
    apkenv_audio_register(sdl_audio);
    apkenv_mixer_register(sdl_mixer);

    /* Publish the version we ACTUALLY got (this may be a fallback), so the
     * hook tables can give the ~68 shared GLES1/GLES2 names to the wrapper that
     * belongs to the live context. Must happen before any apk lib is loaded. */
    apkenv_set_active_gles_version(gles_version);
    /* and point the ES1 wrappers' driver pointers at the live context's device
     * library - the engine's GOT was bound before this context existed. */
    apkenv_gles1_bind_driver(gles_version);

    apkenv_egl_probe("post-SetVideoMode");

    fprintf(stderr, "webos_init: surface %dx%d, gles_version=%d\n",
            priv.screen->w, priv.screen->h, gles_version);

    return 1;
}

static const char *
webos_get_path(enum PlatformPath which)
{
    switch (which) {
        case PLATFORM_PATH_INSTALL_DIRECTORY:
            return "/media/internal/";
        case PLATFORM_PATH_DATA_DIRECTORY:
            return "/media/internal/.apkenv/";
        case PLATFORM_PATH_MODULE_DIRECTORY:
            return "/var/apkenv/modules/";
        default:
            return NULL;
    }
}

static void
webos_get_size(int *width, int *height)
{
    if (width) {
        *width = priv.screen->w;
    }
    if (height) {
        *height = priv.screen->h;
    }
}

/* ---- SDL event-stream instrumentation -------------------------------------
 * Diagnosing: in active gameplay NO touch reaches the app, but a webOS
 * task-switch out-and-back restores it. We need to see, at the SDL/PDL layer,
 * exactly what the event stream does when gameplay goes live:
 *   [SDLHB]  per-N-frame heartbeat: proves input_update keeps being called
 *            (i.e. frames advance / we are NOT stuck in a wait loop), and how
 *            many events were drained since the last heartbeat.
 *   [SDLEV]  every non-motion event (DOWN/UP/ACTIVEEVENT/QUIT/...) verbatim,
 *            so a focus/active change at gameplay entry is impossible to miss.
 * Toggle off with APKENV_SDL_TRACE=0. */
static int sdl_trace = -1;        /* -1 = uninitialised */
static unsigned long iu_calls = 0;
static unsigned long ev_total = 0;
static unsigned long ev_since_hb = 0;
static unsigned long motion_since_hb = 0;

static int
webos_input_update(struct SupportModule *module)
{
    if (sdl_trace < 0) {
        const char *t = getenv("APKENV_SDL_TRACE");
        sdl_trace = (t && t[0] == '0') ? 0 : 1;
    }

    iu_calls++;
    if (sdl_trace && (iu_calls % 120 == 0)) {
        fprintf(stderr, "[SDLHB] input_update call=%lu ev_total=%lu "
                "ev_since=%lu motion_since=%lu\n",
                iu_calls, ev_total, ev_since_hb, motion_since_hb);
        ev_since_hb = 0;
        motion_since_hb = 0;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ev_total++; ev_since_hb++;

        if (e.type == SDL_MOUSEMOTION) {
            motion_since_hb++;
        } else if (sdl_trace) {
            /* log every non-motion event verbatim */
            if (e.type == SDL_ACTIVEEVENT) {
                fprintf(stderr, "[SDLEV] ACTIVEEVENT state=0x%x gain=%d "
                        "(APPACTIVE=%d APPINPUTFOCUS=%d APPMOUSEFOCUS=%d)\n",
                        e.active.state, e.active.gain,
                        !!(e.active.state & SDL_APPACTIVE),
                        !!(e.active.state & SDL_APPINPUTFOCUS),
                        !!(e.active.state & SDL_APPMOUSEFOCUS));
            } else {
                fprintf(stderr, "[SDLEV] type=%d\n", e.type);
            }
        }

        if (e.type == SDL_MOUSEBUTTONDOWN) {
            int f = e.button.which;
            if (f >= 0 && f < WEBOS_MAX_FINGERS) finger_down[f] = 1;
            module->input(module, ACTION_DOWN, e.button.x, e.button.y, f);
        } else if (e.type == SDL_MOUSEBUTTONUP) {
            int f = e.button.which;
            if (f >= 0 && f < WEBOS_MAX_FINGERS) finger_down[f] = 0;
            module->input(module, ACTION_UP, e.button.x, e.button.y, f);
        } else if (e.type == SDL_MOUSEMOTION) {
            int f = e.motion.which;
            /* Gate MOVE on the finger actually being down — SDL emits motion
             * with no button held, which otherwise corrupts the finger map. */
            if (f >= 0 && f < WEBOS_MAX_FINGERS && finger_down[f]) {
                module->input(module, ACTION_MOVE, e.motion.x, e.motion.y, f);
            }
        } else if (e.type == SDL_QUIT) {
            return 1;
        } else if (e.type == SDL_ACTIVEEVENT) {
            if (e.active.state == SDL_APPACTIVE && e.active.gain == 0) {
                fprintf(stderr, "[SDLEV] -> module->pause (APPACTIVE lost), "
                        "entering wait loop (touch dropped until regained)\n");
                module->pause(module);
                while (1) {
                    SDL_WaitEvent(&e);
                    if (e.type == SDL_ACTIVEEVENT) {
                        if (e.active.state == SDL_APPACTIVE && e.active.gain == 1) {
                            break;
                        }
                    } else if (e.type == SDL_QUIT) {
                        return 1;
                    }
                }
                fprintf(stderr, "[SDLEV] -> module->resume (APPACTIVE regained)\n");
                module->resume(module);
            }
        }
    }

    return 0;
}

static int
webos_get_orientation(void)
{
    /* Device natural orientation is LANDSCAPE. A portrait game sets
     * current_orientation = PORTRAIT in its module; the mismatch triggers
     * apkenv's viewport/scissor/projection rotation hooks. */
    return ORIENTATION_LANDSCAPE;
}

static void
webos_request_text_input(int is_password, const char *text,
        text_callback_t callback, void *user_data)
{
    fprintf(stderr, "webos: request_text_input not implemented\n");
    callback(NULL, user_data);
}

/* ---- frame grab -----------------------------------------------------------
 * "The screen is purple" is not a measurement, and there is no screenshot tool
 * on webOS 3.0.5 for a GL app (the compositor owns the panel; /dev/fb0 does not
 * hold the accelerated layer). Read the frame back from GL itself instead, so a
 * render bug can be LOOKED at from the workstation:
 *
 *   APKENV_GL_SNAPSHOT=<frame>[,<frame>...]  writes /media/internal/apkenv-snap-<n>.ppm
 *
 * glReadPixels is resolved from the library that owns the live context - the
 * ES1 and ES2 device libs are separate front-ends and only one is correct. */
static void
webos_snapshot(unsigned long frame)
{
    static void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *);
    static int resolved;
    int w = priv.screen ? priv.screen->w : 0;
    int h = priv.screen ? priv.screen->h : 0;
    unsigned char *px, *row;
    char path[128];
    FILE *f;
    int y;

    if (w <= 0 || h <= 0) return;
    if (!resolved) {
        const char *lib = (apkenv_active_gles_version() == 2)
                        ? "libGLESv2.so" : "libGLES_CM.so";
        void *h2 = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
        if (h2 == NULL) h2 = dlopen(lib, RTLD_LAZY);
        if (h2 != NULL)
            *(void **)&read_pixels = dlsym(h2, "glReadPixels");
        resolved = 1;
        fprintf(stderr, "[SNAP] glReadPixels from %s: %p\n", lib, (void *)read_pixels);
    }
    if (read_pixels == NULL) return;

    px = malloc((size_t)w * h * 4);
    if (px == NULL) return;
    /* GL_RGBA / GL_UNSIGNED_BYTE is the one combination ES guarantees. */
    read_pixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);

    snprintf(path, sizeof(path), "/media/internal/apkenv-snap-%lu.ppm", frame);
    f = fopen(path, "wb");
    if (f == NULL) { free(px); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* GL origin is bottom-left; PPM is top-down. */
    for (y = h - 1; y >= 0; y--) {
        int x;
        row = px + (size_t)y * w * 4;
        for (x = 0; x < w; x++)
            fwrite(row + x * 4, 1, 3, f);
    }
    fclose(f);
    free(px);
    fprintf(stderr, "[SNAP] wrote %s (%dx%d)\n", path, w, h);
}

static int
webos_snapshot_wanted(unsigned long frame)
{
    const char *spec = getenv("APKENV_GL_SNAPSHOT");
    const char *p;
    if (spec == NULL || spec[0] == 0) return 0;
    for (p = spec; *p; ) {
        unsigned long v = strtoul(p, (char **)&p, 10);
        if (v == frame) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

static void
webos_update()
{
    static unsigned long frame;

    /* Stage 3: if the game renders into an offscreen portrait FBO, blit it
     * (rotated) to the native landscape framebuffer before the swap. No-op
     * unless module_hacks->render_to_fbo is set. */
    apkenv_fbo_present();

    frame++;
    if (webos_snapshot_wanted(frame))
        webos_snapshot(frame);   /* before the swap: the back buffer still holds it */

    SDL_GL_SwapBuffers();
}

static void
webos_exit()
{
    PDL_Quit();
}

struct PlatformSupport platform_support = {
    webos_init,
    webos_get_path,
    webos_get_size,
    webos_input_update,
    webos_get_orientation,
    webos_request_text_input,
    webos_update,
    webos_exit,
};
