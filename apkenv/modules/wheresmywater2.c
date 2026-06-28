/**
 * apkenv support module — Disney "Where's My Water? 2" (com.disney.WMW2 family)
 * Walaber engine, GLES1, FMOD audio — the SAME engine subsystems as WMW1, driven
 * through a DIFFERENT JNI layer (Disney's "GameLib Bridge", 109 static exports
 * under com.disney.GameLib.Bridge.* vs WMW1's ~14 WMWRenderer.* methods).
 *
 * This is the 2nd-game generalization (plan/STAGE-5): it proves the apkenv
 * subsystems are game-independent —
 *   - the FMOD AudioTrack device pump (audio/fmod_pump.c) reuses verbatim
 *     (WMW2 ships the same org.fmod.FMODAudioDevice glue), and
 *   - the GLES1 render-to-portrait-FBO path reuses verbatim.
 * What is new is this per-game module: the bridge entrypoints + init order.
 *
 * Bridge entrypoints (from readelf + baksmali of classes.dex):
 *   WalaberNativeChassis.jniWalaberChassisStartup(String,String,String,int,
 *       String,String,String,String,String)   = master init (paths/locale/cfg)
 *   WalaberNativeChassis.jniWalaberChassisAppPause/AppResume/Shutdown()
 *   Rendering.BridgeRendering.jniRenderInit(int w,int h,float xdpi,float ydpi)
 *   Rendering.BridgeRendering.jniRenderAreaCreated() / jniRenderAreaResized(II)
 *   Rendering.BridgeRendering.jniRenderDrawPreDraw() then jniRenderDrawFrame()
 *   DeviceIO.BridgeTouchHandling.jniTouchBegan/Ended(int,float[],float[],int[])
 *   DeviceIO.BridgeTouchHandling.jniTouchMoved(int,f[],f[],f[],f[],int[])   <- same as WMW1
 *   DeviceIO.BridgeSensorHandling.jniAccelerometerChanged(float,float,float)
 *   AppEvents.BridgeAppFocusEvents.jniAppLostFocusPleaseShowPauseMenu()
 *   <Bridge>.jniBridgeInit()  on each bridge (registers its native side)
 *
 * STATUS: first-draft bring-up scaffold, NOT yet device-verified. The
 * device-bring-up unknowns are marked [BRINGUP] below (startup arg semantics,
 * init order, jniRenderInit dpi, instance-method `thiz`, the std::string(NULL)
 * boot bug also present in libwalaber). See plan/STAGE-5 + the port notes.
 **/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "common.h"
#include "../audio/fmod_pump.h"

/* ---- bridge entrypoint signatures ---- */
typedef void (*w2_startup_t)(JNIEnv *, jobject, jstring, jstring, jstring, jint,
                             jstring, jstring, jstring, jstring, jstring) SOFTFP;
typedef void (*w2_renderinit_t)(JNIEnv *, jobject, jint, jint, jfloat, jfloat) SOFTFP;
typedef void (*w2_resized_t)(JNIEnv *, jobject, jint, jint) SOFTFP;
typedef void (*w2_void_t)(JNIEnv *, jobject) SOFTFP;
typedef void (*w2_touch3_t)(JNIEnv *, jobject, jint, jfloatArray, jfloatArray, jintArray) SOFTFP;
typedef void (*w2_touchmoved_t)(JNIEnv *, jobject, jint, jfloatArray, jfloatArray,
                                jfloatArray, jfloatArray, jintArray) SOFTFP;
typedef void (*w2_accel_t)(JNIEnv *, jobject, jfloat, jfloat, jfloat) SOFTFP;

#define W2_MAX_FINGERS 5
typedef struct { int down; float x, y, px, py; } W2Finger;

/* Bridges whose jniBridgeInit we call at startup (chassis first). The analytics/
 * net/ads/social bridges are intentionally NOT inited (no Dalvik to back them);
 * if the engine hard-requires one, add it here. [BRINGUP] */
#define W2_BR(sym) "Java_com_disney_GameLib_Bridge_" sym "_jniBridgeInit"

struct SupportModulePriv {
    jni_onload_t   JNI_OnLoad;
    w2_startup_t   chassisStartup;
    w2_void_t      chassisAppPause, chassisAppResume, chassisShutdown;
    w2_renderinit_t renderInit;
    w2_void_t      renderAreaCreated, renderDrawPreDraw, renderDrawFrame;
    w2_resized_t   renderAreaResized;
    w2_touch3_t    touchBegan, touchEnded;
    w2_touchmoved_t touchMoved;
    w2_accel_t     accelChanged;
    w2_void_t      appLostFocus;

    struct GlobalState *global;
    int   width, height, frame;
    W2Finger fingers[W2_MAX_FINGERS];
    int   multitouch, audio, feed_accel;
    float gx, gy;
};
static struct SupportModulePriv wheresmywater2_priv;

/* call jniBridgeInit on a named bridge if present (best-effort) */
static void
w2_bridge_init(struct SupportModule *self, const char *sym)
{
    w2_void_t f = (w2_void_t)LOOKUP_M(sym);
    if (f) f(ENV_M, (jobject)GLOBAL_M);
}

static int
wheresmywater2_try_init(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    p->JNI_OnLoad      = (jni_onload_t)LOOKUP_M("JNI_OnLoad");
    p->chassisStartup  = (w2_startup_t)LOOKUP_M("WalaberNativeChassis_jniWalaberChassisStartup");
    p->chassisAppPause = (w2_void_t)LOOKUP_M("WalaberNativeChassis_jniWalaberChassisAppPause");
    p->chassisAppResume= (w2_void_t)LOOKUP_M("WalaberNativeChassis_jniWalaberChassisAppResume");
    p->chassisShutdown = (w2_void_t)LOOKUP_M("WalaberNativeChassis_jniWalaberChassisShutdown");
    p->renderInit      = (w2_renderinit_t)LOOKUP_M("BridgeRendering_jniRenderInit");
    p->renderAreaCreated = (w2_void_t)LOOKUP_M("BridgeRendering_jniRenderAreaCreated");
    p->renderAreaResized = (w2_resized_t)LOOKUP_M("BridgeRendering_jniRenderAreaResized");
    p->renderDrawPreDraw = (w2_void_t)LOOKUP_M("BridgeRendering_jniRenderDrawPreDraw");
    p->renderDrawFrame   = (w2_void_t)LOOKUP_M("BridgeRendering_jniRenderDrawFrame");
    p->touchBegan      = (w2_touch3_t)LOOKUP_M("BridgeTouchHandling_jniTouchBegan");
    p->touchEnded      = (w2_touch3_t)LOOKUP_M("BridgeTouchHandling_jniTouchEnded");
    p->touchMoved      = (w2_touchmoved_t)LOOKUP_M("BridgeTouchHandling_jniTouchMoved");
    p->accelChanged    = (w2_accel_t)LOOKUP_M("BridgeSensorHandling_jniAccelerometerChanged");
    p->appLostFocus    = (w2_void_t)LOOKUP_M("jniAppLostFocusPleaseShowPauseMenu");
    p->global = GLOBAL_M;

    /* activate only for the WMW2 apk (libwalaber): the essential entrypoints resolve */
    return (p->JNI_OnLoad && p->chassisStartup && p->renderInit &&
            p->renderDrawFrame && p->touchBegan);
}

static void
wheresmywater2_init(struct SupportModule *self, int width, int height, const char *home)
{
    struct SupportModulePriv *p = self->priv;
    JNIEnv *env = ENV_M;

    p->width = width;
    p->height = height;
    p->frame = 0;
    memset(p->fingers, 0, sizeof(p->fingers));

    const char *mt = getenv("APKENV_WMW2_MULTITOUCH");
    p->multitouch = (mt && mt[0] == '1') ? 1 : 0;
    const char *fa = getenv("APKENV_WMW2_ACCEL");
    p->feed_accel = (fa && fa[0] == '0') ? 0 : 1;
    p->gx = 0.0f; p->gy = -9.81f;
    fprintf(stderr, "wheresmywater2_init: %dx%d home=%s\n", width, height, home);

    /* GLES1 render-to-portrait-FBO (reused verbatim from WMW1 / Stage 3): the
     * engine renders its whole frame into an offscreen 768x1024 portrait FBO and
     * the platform rotates it once at present. Default ON; APKENV_WMW2_FBO=0
     * falls back to the landscape surface. [BRINGUP: confirm WMW2 is portrait] */
    const char *use_fbo = getenv("APKENV_WMW2_FBO");
    if (!use_fbo || use_fbo[0] != '0') {
        GLOBAL_M->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;
        GLOBAL_M->module_hacks->gles_viewport_hack = 0;
        GLOBAL_M->module_hacks->glOrthof_rotation_hack = 0;
        GLOBAL_M->module_hacks->render_to_fbo = 1;
        GLOBAL_M->module_hacks->fbo_w = 768;
        GLOBAL_M->module_hacks->fbo_h = 1024;
        fprintf(stderr, "wheresmywater2_init: RENDER-TO-PORTRAIT-FBO mode\n");
    }

    p->JNI_OnLoad(VM_M, NULL);

    /* (1) register the native side of each bridge we drive (chassis first).
     * [BRINGUP] order/superset is a best guess from the Android init path. */
    w2_bridge_init(self, W2_BR("WalaberNativeChassis"));
    w2_bridge_init(self, W2_BR("Rendering_BridgeRendering"));
    w2_bridge_init(self, W2_BR("DeviceIO_BridgeTouchHandling"));
    w2_bridge_init(self, W2_BR("DeviceIO_BridgeSensorHandling"));
    w2_bridge_init(self, W2_BR("AppEvents_BridgeAppFocusEvents"));
    w2_bridge_init(self, W2_BR("AppEvents_BridgeAudioAppInfo"));
    w2_bridge_init(self, W2_BR("AppEvents_BridgeGameFlowEvents"));

    /* (2) master init. The 9 args, mapped from the deobfuscated `as` storage-paths
     * helper (which feeds WalaberChassisStartup in d.smali):
     *   1 sourceDir   = the apk path            (asset source)
     *   2 dataDir     = WRITABLE dir            (progress.db / perry.db live here)
     *   3 cacheDir    = WRITABLE dir            (getCacheDir)
     *   4 int         = At.a()                  (config; 0)
     *   5 language    6 country
     *   7 aq.a()      = version/id string       ("" tolerated)
     *   8 d()         = WRITABLE dir            (cache-ish)
     *   9 e()         = WRITABLE dir            (getExternalFilesDir)
     * The writable args MUST be real writable directories — passing the apk path
     * made the engine write its DBs into the apk path and corrupt the heap. We
     * use apkenv's data dir (`home`, already created + writable) for all of them. */
    jstring apk  = (*env)->NewStringUTF(env, GLOBAL_M->apk_filename);
    jstring data = (*env)->NewStringUTF(env, home);
    jstring cache= (*env)->NewStringUTF(env, home);
    jstring lang = (*env)->NewStringUTF(env, "en");
    jstring ctry = (*env)->NewStringUTF(env, "US");
    jstring ver  = (*env)->NewStringUTF(env, "");
    jstring ext  = (*env)->NewStringUTF(env, home);
    jstring ext2 = (*env)->NewStringUTF(env, home);
    p->chassisStartup(env, (jobject)GLOBAL_M, apk, data, cache, 0,
                      lang, ctry, ver, ext, ext2);

    /* (3) renderer bring-up. jniRenderInit(widthPx, heightPx, physWmm, physHmm) —
     * the engine's RenderInit derives the last two floats as PHYSICAL SIZE IN
     * MILLIMETRES (widthPx/dpi*25.4), and uses them to scale the level render.
     * Passing dpi (132) made it think the panel was 132x132mm and shrink the
     * level to 0.75 (anchored bottom-left). TouchPad is ~132 dpi → real size is
     * ~197x148mm. [tune APKENV_WMW2_DPI if the in-level scale is off] */
    {
        const char *d = getenv("APKENV_WMW2_DPI");
        float dpi = d ? (float)atof(d) : 132.0f;
        float mmw = (float)width  / dpi * 25.4f;
        float mmh = (float)height / dpi * 25.4f;
        fprintf(stderr, "wheresmywater2_init: renderInit(%d,%d, %.1fmm,%.1fmm) dpi=%.0f\n",
                width, height, mmw, mmh, dpi);
        p->renderInit(env, (jobject)GLOBAL_M, width, height, mmw, mmh);
    }
    if (p->renderAreaCreated) p->renderAreaCreated(env, (jobject)GLOBAL_M);
    if (p->renderAreaResized) p->renderAreaResized(env, (jobject)GLOBAL_M, width, height);

    /* (4) resume + audio */
    if (p->chassisAppResume) p->chassisAppResume(env, (jobject)GLOBAL_M);

    const char *au = getenv("APKENV_WMW2_AUDIO");
    p->audio = (au && au[0] == '0') ? 0 : 1;
    if (p->audio) apkenv_fmod_pump_start(GLOBAL_M);
}

/* ---- touch (identical Walaber interface to WMW1) ---- */
static void
w2_normalize(struct SupportModulePriv *p, int rawx, int rawy, float *fx, float *fy)
{
    float W = (float)(p->width  ? p->width  : 1024);
    float H = (float)(p->height ? p->height :  768);
    *fx = (H - (float)rawy) / H;   /* invert the 90deg portrait render rotation */
    *fy = (float)rawx / W;
}

static jfloatArray
w2_fa(JNIEnv *env, const float *v, int n)
{ jfloatArray a = (*env)->NewFloatArray(env, n); (*env)->SetFloatArrayRegion(env, a, 0, n, v); return a; }
static jintArray
w2_ia(JNIEnv *env, const jint *v, int n)
{ jintArray a = (*env)->NewIntArray(env, n); (*env)->SetIntArrayRegion(env, a, 0, n, v); return a; }

static int
w2_collect(struct SupportModulePriv *p, int changed,
           float *xs, float *ys, float *pxs, float *pys, jint *ids)
{
    if (!p->multitouch) {
        W2Finger *f = &p->fingers[changed];
        xs[0]=f->x; ys[0]=f->y; pxs[0]=f->px; pys[0]=f->py; ids[0]=0;
        return 1;
    }
    int n = 0;
    for (int i = 0; i < W2_MAX_FINGERS; i++) if (p->fingers[i].down) {
        xs[n]=p->fingers[i].x; ys[n]=p->fingers[i].y;
        pxs[n]=p->fingers[i].px; pys[n]=p->fingers[i].py; ids[n]=i; n++;
    }
    return n;
}

static void
wheresmywater2_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    struct SupportModulePriv *p = self->priv;
    JNIEnv *env = ENV_M;
    if (finger < 0 || finger >= W2_MAX_FINGERS) finger = 0;
    W2Finger *f = &p->fingers[finger];

    float fx, fy; w2_normalize(p, x, y, &fx, &fy);
    float xs[W2_MAX_FINGERS], ys[W2_MAX_FINGERS], pxs[W2_MAX_FINGERS], pys[W2_MAX_FINGERS];
    jint ids[W2_MAX_FINGERS];
    int n;

    if (event == ACTION_DOWN) {
        f->down = 1; f->px = f->x = fx; f->py = f->y = fy;
        n = w2_collect(p, finger, xs, ys, pxs, pys, ids);
        p->touchBegan(env, (jobject)GLOBAL_M, n, w2_fa(env,xs,n), w2_fa(env,ys,n), w2_ia(env,ids,n));
    } else if (event == ACTION_MOVE) {
        f->px = f->x; f->py = f->y; f->x = fx; f->y = fy; f->down = 1;
        n = w2_collect(p, finger, xs, ys, pxs, pys, ids);
        if (p->touchMoved)
            p->touchMoved(env, (jobject)GLOBAL_M, n, w2_fa(env,xs,n), w2_fa(env,ys,n),
                          w2_fa(env,pxs,n), w2_fa(env,pys,n), w2_ia(env,ids,n));
    } else if (event == ACTION_UP) {
        f->px = f->x; f->py = f->y; f->x = fx; f->y = fy;
        n = w2_collect(p, finger, xs, ys, pxs, pys, ids);
        p->touchEnded(env, (jobject)GLOBAL_M, n, w2_fa(env,xs,n), w2_fa(env,ys,n), w2_ia(env,ids,n));
        f->down = 0;
    }
}

static void
wheresmywater2_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    (void)self; (void)event; (void)keycode; (void)unicode;
}

static void
wheresmywater2_update(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    JNIEnv *env = ENV_M;
    p->frame++;

    if (p->feed_accel && p->accelChanged)
        p->accelChanged(env, (jobject)GLOBAL_M, p->gx, p->gy, 0.0f);

    if (p->renderDrawPreDraw) p->renderDrawPreDraw(env, (jobject)GLOBAL_M);
    p->renderDrawFrame(env, (jobject)GLOBAL_M);
}

static void
wheresmywater2_deinit(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    if (p->audio) apkenv_fmod_pump_stop();
    if (p->chassisShutdown) p->chassisShutdown(ENV_M, (jobject)GLOBAL_M);
}

static void
wheresmywater2_pause(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    if (p->audio) apkenv_fmod_pump_stop();
    if (p->appLostFocus)    p->appLostFocus(ENV_M, (jobject)GLOBAL_M);
    if (p->chassisAppPause) p->chassisAppPause(ENV_M, (jobject)GLOBAL_M);
}

static void
wheresmywater2_resume(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    if (p->chassisAppResume) p->chassisAppResume(ENV_M, (jobject)GLOBAL_M);
    if (p->audio) apkenv_fmod_pump_start(GLOBAL_M);
}

static int
wheresmywater2_requests_exit(struct SupportModule *self)
{
    (void)self;
    return 0;
}

APKENV_MODULE(wheresmywater2, MODULE_PRIORITY_GAME_VERSION)
