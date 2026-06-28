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

#include <SDL.h>
#include <PDL.h>

#include "common/sdl_accelerometer_impl.h"
#include "common/sdl_audio_impl.h"
#include "common/sdl_mixer_impl.h"

#include "common/input_transform.h"

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
    /* PDL FIRST — before SDL. This is the 3-layer-compositor requirement. */
    PDL_Init(0);
    PDL_SetTouchAggression(PDL_AGGRESSION_MORETOUCHES);
    PDL_GesturesEnable(PDL_FALSE);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "webos_init: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gles_version);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* 0,0,0 -> keep the device's native landscape mode (1024x768). */
    priv.screen = SDL_SetVideoMode(0, 0, 0, SDL_OPENGLES | SDL_FULLSCREEN);
    if (priv.screen == NULL) {
        fprintf(stderr, "webos_init: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_ShowCursor(0);

    memset(finger_down, 0, sizeof(finger_down));

    apkenv_accelerometer_register(sdl_accelerometer);
    apkenv_audio_register(sdl_audio);
    apkenv_mixer_register(sdl_mixer);

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

static void
webos_update()
{
    /* Stage 3: if the game renders into an offscreen portrait FBO, blit it
     * (rotated) to the native landscape framebuffer before the swap. No-op
     * unless module_hacks->render_to_fbo is set. */
    apkenv_fbo_present();
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
