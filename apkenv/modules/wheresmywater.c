/**
 * apkenv support module — Disney "Where's My Water?" (com.disney.WMW)
 * Walaber engine, GLES1, FMOD audio.
 *
 * Reconstructed from BUILD-STATE / android-port-shim.md notes. The engine's
 * Java is NOT run; we call its JNI native entrypoints directly.
 *
 * JNI exports (confirmed via readelf + disassembly):
 *   WMWRenderer_rendererInit(String sourceDir, String dataDir, Context)
 *   WMWRenderer_rendererResized(int w, int h)
 *   WMWRenderer_notifyScreenResized(int w, int h)   (no-op in this build)
 *   WMWRenderer_rendererDrawFrame()                  (logic + render)
 *   WMWRenderer_rendererReloadContextData()
 *   WMWRenderer_rendererTouchBegan(int n, float[] xs, float[] ys, int[] ids)
 *   WMWRenderer_rendererTouchEnded(int n, float[] xs, float[] ys, int[] ids)
 *   WMWRenderer_rendererTouchMoved(int n, float[] xs, float[] ys,
 *                                  float[] prevXs, float[] prevYs, int[] ids)
 *   BaseActivity_{onGamePause,onGameResume,onLostFocus,onRegainedFocus,
 *                 accelerometerChanged(f,f,f), backKeyPressed}
 *
 * COORDINATES: the engine wants NORMALIZED 0..1 touch coords (its WMWView
 * .copyTouches does getX(i)/getWidth(), getY(i)/getHeight()). We also invert
 * apkenv's 90-deg portrait render rotation. On a 1024x768 landscape surface:
 *     fx = (H - rawY) / H ;  fy = rawX / W       (corner-calibrated)
 *
 * TOUCH MODES (runtime, no rebuild needed):
 *   default            -> LEGACY single-finger path: pin finger id 0, count=1.
 *                         This is the path that made the MENUS interactive.
 *   APKENV_WMW_MULTITOUCH=1 -> real per-finger aggregation (Began/Moved/Ended
 *                         dispatch ALL active fingers with their real ids).
 *                         For the in-game swipe/carve (Screen_WaterTest).
 * Both paths log every dispatch to stderr (-> /tmp/wmw.log).
 **/

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "../audio/fmod_pump.h"

/* Audio: drive FMOD's AudioTrack device pump in C (no Dalvik to run its Java
 * thread). Default ON; APKENV_WMW_AUDIO=0 disables. General code lives in
 * audio/fmod_pump.c + audio/audiotrack.c. */
static int g_audio = 1;

typedef void (*wmw_init_t)(JNIEnv *env, jobject obj, jstring sourceDir, jstring dataDir, jobject context) SOFTFP;
typedef void (*wmw_resized_t)(JNIEnv *env, jobject obj, jint w, jint h) SOFTFP;
typedef void (*wmw_drawframe_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*wmw_reload_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*wmw_touch3_t)(JNIEnv *env, jobject obj, jint n, jfloatArray xs, jfloatArray ys, jintArray ids) SOFTFP;
typedef void (*wmw_touchmoved_t)(JNIEnv *env, jobject obj, jint n, jfloatArray xs, jfloatArray ys, jfloatArray pxs, jfloatArray pys, jintArray ids) SOFTFP;
typedef void (*wmw_void_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*wmw_accel_t)(JNIEnv *env, jobject obj, jfloat x, jfloat y, jfloat z) SOFTFP;

#define WMW_MAX_FINGERS 5

typedef struct {
    int down;
    float x, y;      /* current normalized */
    float px, py;    /* previous normalized (for Moved) */
} WmwFinger;

/* Raw SDL touch event, queued from the main thread to the "UI thread". */
typedef struct { int event, x, y, finger; } TouchEvt;
#define WMW_TOUCHQ 512

struct SupportModulePriv {
    jni_onload_t JNI_OnLoad;
    wmw_init_t rendererInit;
    wmw_resized_t rendererResized;
    wmw_resized_t notifyScreenResized;
    wmw_drawframe_t rendererDrawFrame;
    wmw_reload_t rendererReloadContextData;
    wmw_touch3_t rendererTouchBegan;
    wmw_touch3_t rendererTouchEnded;
    wmw_touchmoved_t rendererTouchMoved;
    wmw_void_t onGamePause;
    wmw_void_t onGameResume;
    wmw_void_t onLostFocus;
    wmw_void_t onRegainedFocus;
    wmw_accel_t accelerometerChanged;
    wmw_void_t backKeyPressed;

    struct GlobalState *global;
    int width, height;          /* surface size (landscape, e.g. 1024x768) */
    int multitouch;             /* runtime mode flag */
    WmwFinger fingers[WMW_MAX_FINGERS];  /* touched only by the dispatch thread */
    int frame;                  /* frame counter for instrumentation */

    /* "UI thread" touch split (mimics Android onTouchEvent vs onDrawFrame
     * running on different threads). When enabled, the main thread enqueues
     * raw SDL touch events here and a dedicated thread calls rendererTouch*,
     * so the engine's internal UI/GL-thread queue handoff has two real threads. */
    int use_uithread;
    pthread_t ui_thread;
    pthread_mutex_t q_lock;
    pthread_cond_t q_cond;
    TouchEvt q[WMW_TOUCHQ];
    int q_head, q_tail;
    volatile int ui_running;

    /* Probe of Walaber::ScreenManager::mCurrentTransition. ScreenManager::
     * touchMoved drops ALL touches while this is non-NULL (a transition is
     * animating). Hypothesis: it stays stuck non-NULL in Screen_WaterTest.
     * Address computed from the load slide (see init). */
    void **mtrans;

    /* Screen-stack probe: identify the active (top) screen in-level by its
     * vtable pointer. mScreenStack is std::vector<GameScreen*> {start,finish,..}. */
    unsigned long slide;
    char ***sstack;            /* &mScreenStack (-> [start, finish, end]) */
    unsigned long wt_vtable;   /* runtime vtable ptr of Screen_WaterTest */
    int force_input;           /* APKENV_WMW_FORCEINPUT: force wm gate on in-level */

    /* Experiment: call Screen_WaterTest::regainedTop() once after entering the
     * level (it enables the interactive widgets, like the post-pause path). */
    int call_regain;           /* APKENV_WMW_CALLREGAIN */
    int regain_done;
    int wt_active_frames;
    void (*wt_regainedTop)(void *self) SOFTFP;

    /* Experiment: dispatch BEGAN twice so the 2nd touchDown finds the
     * just-created FingerInfo and runs the hit-test -> catches the finger. */
    int double_down;           /* APKENV_WMW_DOUBLEDOWN */
};

/* Screen_WaterTest object vtable POINTER (link): symbol _ZTV..+8 = 0x46b4b8 */
#define WMW_LINK_WT_vtableptr        0x0046b4b8UL

/* libwmw link-time vaddrs (from readelf on the shipped lib) */
#define WMW_LINK_rendererTouchBegan  0x0010f488UL
#define WMW_LINK_mCurrentTransition  0x00470ec0UL
#define WMW_LINK_mScreenStack        0x00470e78UL
#define WMW_LINK_WT_touchDown        0x003aea8cUL
#define WMW_LINK_WT_touchMoved       0x003aec0cUL
#define WMW_LINK_WT_touchUp          0x003aeabcUL
#define WMW_LINK_AABB_contains       0x0025d168UL
#define WMW_LINK_WT_regainedTop      0x003ac2ecUL
#define WMW_LINK_World_touchDown     0x002c3c84UL
#define WMW_LINK_World_touchMoved    0x002c3c38UL
#define WMW_LINK_World_touchUp       0x002c3bf4UL
#define WMW_LINK_WT_handleEvent      0x003b0454UL
#define WMW_LINK_FC_acceptFinger     0x001fe428UL
#define WMW_LINK_FC_acceptEntered    0x001fe7a4UL
#define WMW_LINK_WM_touchDown        0x001edc10UL
#define WMW_LINK_WM_update           0x001ece38UL
#define WMW_LINK_WM_addWidget        0x001ea43cUL
#define WMW_LINK_CurrentDeviceOrientation 0x0046f97cUL

/* ---- inline-hook probe: confirm Screen_WaterTest::touch{Down,Moved} actually
 * execute in-level. Vector2 = {float x,y} (8 bytes, passed in r2,r3). ---- */
typedef struct { float x, y; } V2;
/* Walaber::Vector2 is non-trivial -> passed BY POINTER (r2/r3 hold addresses). */
static void (*wt_orig_touchDown)(void *self, int id, V2 *p) SOFTFP;
static void (*wt_orig_touchMoved)(void *self, int id, V2 *p, V2 *pv) SOFTFP;
static void (*wt_orig_touchUp)(void *self, int id, V2 *p) SOFTFP;

/* AABB::contains logging gate (armed by the touchDown hook). */
static volatile int g_aabb_log = 0;
static float g_last_px, g_last_py;
static unsigned long g_slide = 0;   /* libwmw load slide, for vtable->class id */
static int g_enter_catch = 0;       /* APKENV_WMW_ENTERCATCH: set FingerCatcher+0xe0=1 */
static int g_force_catch = 0;       /* APKENV_WMW_FORCECATCH: call _acceptFinger ourselves */
static int g_rehit = 0;             /* APKENV_WMW_REHIT: re-invoke touchDown from update() (FAILED: wrong context) */
static int g_rehit2 = 0;            /* APKENV_WMW_REHIT2: re-invoke touchDown from inside the touchDown hook (right context) */
static int g_in_level = 0;          /* set by update when Screen_WaterTest is active */
static int g_feed_accel = 1;        /* feed accelerometer each frame (gravity); we never did before */
static float g_gx = 0.0f, g_gy = -9.81f;  /* upright-portrait gravity guess; env-tunable for sign */
static int g_orient = -1;           /* APKENV_WMW_ORIENT: override GameSettings::CurrentDeviceOrientation (-1=leave) */
static int g_forced = 0;            /* already forced for the current finger-down */
static void *g_catcher = NULL;      /* live Widget_FingerCatcher* (from handleEvent) */
static void (*g_acceptFinger)(void *self, int id, void *fi) SOFTFP;
/* Widget_FingerCatcher object vtable pointer (link): _ZTV..+8 = 0x469618 */
#define WMW_LINK_FC_vtableptr 0x00469618UL
static int (*aabb_orig_contains)(void *aabb, V2 *pt) SOFTFP;

static void SOFTFP
wt_hook_touchDown(void *self, int id, V2 *p)
{
    /* self = Screen_WaterTest*; its WidgetManager is at self+0x50, gate @wm+0x60 */
    char *wm = *(char **)((char *)self + 0x50);
    int gate = wm ? *(unsigned char *)(wm + 0x60) : -1;
    fprintf(stderr, "[WMWHOOK] touchDown id=%d (%.3f,%.3f) wm=%p gate=%d\n",
            id, p ? p->x : 0, p ? p->y : 0, (void *)wm, gate);
    /* arm the AABB::contains logger; leave it armed so contains calls during
     * the subsequent touchUp/touchMoved (where the hit-test actually happens)
     * are captured too. */
    if (p) { g_last_px = p->x; g_last_py = p->y; }
    g_aabb_log = 24;
    wt_orig_touchDown(self, id, p);
}

static void SOFTFP
wt_hook_touchUp(void *self, int id, V2 *p)
{
    char *wm = *(char **)((char *)self + 0x50);
    int gate = wm ? *(unsigned char *)(wm + 0x60) : -1;
    fprintf(stderr, "[WMWHOOK] touchUp id=%d (%.3f,%.3f) wm=%p gate=%d\n",
            id, p ? p->x : 0, p ? p->y : 0, (void *)wm, gate);
    g_aabb_log = 24;
    wt_orig_touchUp(self, id, p);
}

/* Final-handler hooks: do the carve (World) and the button-event (screen)
 * handlers actually get reached? */
static void (*world_orig_touchDown)(void *self, int id, V2 *pos) SOFTFP;
static void (*world_orig_touchMoved)(void *self, int id, V2 *pos, V2 *prev) SOFTFP;
static int (*wt_orig_handleEvent)(void *self, int a, void *ret, void *w) SOFTFP;
static void (*world_orig_touchUp)(void *self, int id, V2 *pos) SOFTFP;
static int world_tu_count = 0;
static void SOFTFP
world_hook_touchUp(void *self, int id, V2 *pos)
{
    if ((world_tu_count++ % 15) == 0)
        fprintf(stderr, "[WMWWORLD] World::handleTouchUp id=%d (%.1f,%.1f)\n",
                id, pos ? pos->x : 0, pos ? pos->y : 0);
    world_orig_touchUp(self, id, pos);
}

static void SOFTFP
world_hook_touchDown(void *self, int id, V2 *pos)
{
    fprintf(stderr, "[WMWWORLD] World::handleTouchDown id=%d (%.1f,%.1f)\n",
            id, pos ? pos->x : 0, pos ? pos->y : 0);
    world_orig_touchDown(self, id, pos);
}
static int world_wm_count = 0;
static void SOFTFP
world_hook_touchMoved(void *self, int id, V2 *pos, V2 *prev)
{
    if ((world_wm_count++ % 15) == 0)
        fprintf(stderr, "[WMWWORLD] World::handleTouchMoved id=%d (%.1f,%.1f)\n",
                id, pos ? pos->x : 0, pos ? pos->y : 0);
    world_orig_touchMoved(self, id, pos, prev);
}
static int g_he_count = 0;
static int SOFTFP
wt_hook_handleEvent(void *self, int a, void *ret, void *w)
{
    /* For a==5 (gameplay touch), [ret+12] is the touch-point count; <=0 routes
     * to the touch-up path and the carve loop is skipped. */
    int cnt = ret ? *(int *)((char *)ret + 12) : -999;
    int b0  = ret ? *(unsigned char *)ret : 0;
    unsigned long vt = w ? (*(unsigned long *)w - g_slide) : 0;
    if (w && vt == WMW_LINK_FC_vtableptr) g_catcher = w;  /* remember the catcher */
    /* experiment: enable the FingerCatcher widget for the enter-detection.
     * 0x1ed0a0 skips widgets whose +0x60==0; acceptNewFingerEntered gates on
     * +0xe0. Set BOTH and log their pre-set values once. */
    if (g_enter_catch && w && vt == WMW_LINK_FC_vtableptr) {
        unsigned char *e60 = (unsigned char *)((char *)w + 0x60);
        unsigned char *ee0 = (unsigned char *)((char *)w + 0xe0);
        static int logged_fc = 0;
        if (!logged_fc) { logged_fc = 1;
            fprintf(stderr, "[WMWFC] catcher=%p pre +0x60=%d +0xe0=%d -> setting both=1\n",
                    w, *e60, *ee0); }
        *e60 = 1;
        *ee0 = 1;
    }
    if ((g_he_count++ % 10) == 0)
        fprintf(stderr, "[WMWEVENT] handleEvent a=%d widget=%p vtable_link=0x%lx ret[0]=%d count[+12]=%d\n",
                a, w, vt, b0, cnt);
    return wt_orig_handleEvent(self, a, ret, w);
}

/* WidgetManager::touchDown probe: on each down, does the finger id already
 * exist in the manager's FingerInfo map (-> hit-test/catch path) or not
 * (-> create path, never catches)? Walk the std::map<int,FingerInfo*> at
 * wm+0x28 (node: color@0 parent@4 left@8 right@12 key@16 value@20). */
static void (*wm_orig_touchDown)(void *self, int id, V2 *pos) SOFTFP;
static void SOFTFP
wm_hook_touchDown(void *self, int id, V2 *pos)
{
    char *wm = self;
    char *node = *(char **)(wm + 0x2c);   /* tree root (header->parent) */
    char *found = NULL;
    int guard = 0;
    while (node && guard++ < 64) {
        int key = *(int *)(node + 16);
        if (id < key)       node = *(char **)(node + 8);   /* left */
        else if (id > key)  node = *(char **)(node + 12);  /* right */
        else { found = node; break; }
    }
    if (found) {
        char *fi = *(char **)(found + 20);            /* FingerInfo* */
        void *cap = fi ? *(void **)(fi + 20) : (void *)-1; /* captured widget */
        fprintf(stderr, "[WMWWMTD] id=%d FOUND fi=%p captured=%p -> %s\n",
                id, (void *)fi, cap,
                (cap == NULL) ? "HIT-TEST path" : "cleanup/recreate");
    } else {
        fprintf(stderr, "[WMWWMTD] id=%d NOT-FOUND -> CREATE path (no hit-test, no catch)\n", id);
    }
    wm_orig_touchDown(self, id, pos);   /* 1st: creates FingerInfo if fresh */

    /* REHIT2: on a fresh in-level down, re-invoke touchDown so it now FINDS the
     * just-created FingerInfo and runs the real hit-test -> captures the widget
     * under the finger (button or FingerCatcher) via the engine's own walk. In
     * the correct touch-flow context (unlike the failed update()-based REHIT). */
    if (g_rehit2 && g_in_level && !found) {
        fprintf(stderr, "[WMWREHIT2] re-touchDown(self=%p,id=%d) to trigger hit-test\n",
                self, id);
        wm_orig_touchDown(self, id, pos);
    }
}

/* WidgetManager::update(float dt) — the ONLY path that catches a FRESH (uncaptured)
 * finger (static RE: "Phase 2" walks uncaptured fingers with FingerInfo+0==0, AABB
 * ::contains each widget, then acceptNewFingerDown). dt is provably UNUSED inside.
 * Probe: each in-level frame this hook dumps whether finger id 0 is in the manager's
 * map and, if so, its state (FingerInfo+0) and captured widget (FingerInfo+20) — i.e.
 * does the Phase-2 catch precondition (present + state 0 + uncaptured) actually hold
 * when update() runs? */
static void (*wm_orig_update)(void *self, int dt) SOFTFP;
static int g_upd_count = 0;
static int g_arb_logs = 0;
static int g_killthief = 0;   /* APKENV_WMW_KILLTHIEF: disable the full-screen thief */

/* WidgetManager::addWidget(Widget*, int key) sets widget+0x10 = key (the entered-
 * tree input-priority key; lowest wins). Log the order, key and class of every
 * widget added so we can see why the Swampy PushButton (key 0) outranks the carve
 * FingerCatcher (key 5): content-fixed keys (=> a state-gated widget left active)
 * vs a running order (=> async load-order divergence). */
static void (*wm_orig_addWidget)(void *mgr, void *widget, int key) SOFTFP;
static int g_add_seq = 0;
static void SOFTFP
wm_hook_addWidget(void *mgr, void *widget, int key)
{
    if (g_add_seq < 300) {
        unsigned long vt = widget ? (*(unsigned long *)widget - g_slide) : 0;
        fprintf(stderr, "[WMWADD] seq=%d key=%d mgr=%p widget=%p vtable_link=0x%lx\n",
                g_add_seq, key, mgr, widget, vt);
    }
    g_add_seq++;
    wm_orig_addWidget(mgr, widget, key);
}
static void SOFTFP
wm_hook_update(void *self, int dt)
{
    if (g_in_level) {
        char *node = *(char **)((char *)self + 0x2c);   /* finger-map tree root */
        char *fi = NULL; int guard = 0;
        while (node && guard++ < 64) {
            int key = *(int *)(node + 16);
            if (0 < key)      node = *(char **)(node + 8);
            else if (0 > key) node = *(char **)(node + 12);
            else { fi = *(char **)(node + 20); break; }
        }
        if (fi) {
            char *cap = *(char **)(fi + 20);
            unsigned long capvt = cap ? (*(unsigned long *)cap - g_slide) : 0UL;
            float minx=0, miny=0, maxx=0, maxy=0;
            if (cap) { minx=*(float*)(cap+0x6c); miny=*(float*)(cap+0x70);
                       maxx=*(float*)(cap+0x74); maxy=*(float*)(cap+0x78); }
            int fullscreen = cap && (maxx-minx) > 700.f && (maxy-miny) > 900.f;

            /* KILL-THIEF experiment: a full-screen key-0 PushButton (vt 0x469928)
             * swallows EVERY touch -> carve + HUD dead on all levels. Disable it
             * (+0x60=0, skipped in the contains-walk) and release the captured
             * finger so the carve FingerCatcher / real HUD buttons get it. If
             * this revives carve+HUD, the thief is conclusively the sole blocker. */
            if (g_killthief && cap && capvt == 0x469928 && fullscreen) {
                *(unsigned char *)(cap + 0x60) = 0;
                *(void **)(fi + 20) = NULL;
                static int killed = 0;
                if (killed++ < 8)
                    fprintf(stderr, "[WMWKILL] disabled full-screen thief %p bounds(%.0f,%.0f..%.0f,%.0f) + released finger\n",
                            (void *)cap, minx, miny, maxx, maxy);
            }
            if (g_arb_logs < 40) {
                g_arb_logs++;
                char *fc = (char *)g_catcher;
                fprintf(stderr,
                    "[WMWARB] captured=%p[k=%d lay=%d en=%d vt=0x%lx bnd=%.0f,%.0f..%.0f,%.0f]  catcher=%p[k=%d lay=%d en=%d]\n",
                    (void *)cap, cap ? *(int *)(cap + 0x10) : -999, cap ? *(int *)(cap + 0x54) : -999,
                    cap ? *(unsigned char *)(cap + 0x60) : 255, capvt, minx, miny, maxx, maxy,
                    (void *)fc, fc ? *(int *)(fc + 0x10) : -999, fc ? *(int *)(fc + 0x54) : -999,
                    fc ? *(unsigned char *)(fc + 0x60) : 255);
            }
        } else if ((g_upd_count++ % 120) == 0) {
            fprintf(stderr, "[WMWUPD] finger0 ABSENT\n");
        }
    }
    wm_orig_update(self, dt);
}

/* Is the engine's per-frame enter-detection EVER calling acceptNewFingerEntered
 * (for any widget/finger)? If never, the enter-detection bails before the catch. */
static int (*fc_orig_acceptEntered)(void *self, int id, void *fi) SOFTFP;
static int fc_ae_count = 0;
static int SOFTFP
fc_hook_acceptEntered(void *self, int id, void *fi)
{
    if ((fc_ae_count++ % 8) == 0)
        fprintf(stderr, "[WMWENTER] acceptNewFingerEntered self=%p id=%d fi=%p\n", self, id, fi);
    return fc_orig_acceptEntered(self, id, fi);
}

/* Widget_FingerCatcher::_acceptFinger — is the carve widget ever told to catch
 * our finger? (this=catcher, id, FingerInfo*) */
static void (*fc_orig_acceptFinger)(void *self, int id, void *fi) SOFTFP;
static void SOFTFP
fc_hook_acceptFinger(void *self, int id, void *fi)
{
    fprintf(stderr, "[WMWCATCH] FingerCatcher::_acceptFinger id=%d fingerInfo=%p\n", id, fi);
    fc_orig_acceptFinger(self, id, fi);
}

/* AABB::contains hook, gated to log only the first few calls after a touchDown
 * so we can see which widget bounds are tested vs the touch point. */
static int SOFTFP
aabb_hook_contains(void *aabb, V2 *pt)
{
    if (g_aabb_log > 0) {
        g_aabb_log--;
        float *f = (float *)aabb;
        int valid = ((int *)aabb)[4];
        int r = aabb_orig_contains(aabb, pt);
        fprintf(stderr, "[WMWAABB] pt(%.1f,%.1f) box[min(%.1f,%.1f) max(%.1f,%.1f) valid=%d] -> %s\n",
                pt ? pt->x : 0, pt ? pt->y : 0, f[0], f[1], f[2], f[3], valid,
                r ? "HIT" : "miss");
        return r;
    }
    return aabb_orig_contains(aabb, pt);
}

static int wt_tm_count = 0;
static void SOFTFP
wt_hook_touchMoved(void *self, int id, V2 *p, V2 *pv)
{
    if ((wt_tm_count++ % 20) == 0) {
        char *wm = *(char **)((char *)self + 0x50);
        int gate = wm ? *(unsigned char *)(wm + 0x60) : -1;
        fprintf(stderr, "[WMWHOOK] touchMoved id=%d (%.3f,%.3f) wm=%p gate=%d\n",
                id, p ? p->x : 0, p ? p->y : 0, (void *)wm, gate);
    }
    g_aabb_log = 24;   /* capture drag hit-test (carve) */
    wt_orig_touchMoved(self, id, p, pv);
}

/* Install an ARM inline detour at `target`; returns a trampoline that runs the
 * overwritten prologue then continues at target+8. Prologue MUST be 2
 * position-independent instructions (verified: push {lr}; sub sp,#..). */
static void *
wmw_install_hook(unsigned long target, void *hookfn)
{
    long pg = sysconf(_SC_PAGESIZE);
    unsigned long pgstart = target & ~(unsigned long)(pg - 1);
    if (mprotect((void *)pgstart, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[WMWHOOK] mprotect failed @0x%lx\n", target);
        return NULL;
    }
    unsigned int *tramp = mmap(NULL, 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        fprintf(stderr, "[WMWHOOK] mmap failed\n");
        return NULL;
    }
    unsigned int *t = (unsigned int *)target;
    tramp[0] = t[0];
    tramp[1] = t[1];
    tramp[2] = 0xe51ff004;                  /* ldr pc, [pc, #-4] */
    tramp[3] = (unsigned int)(target + 8);
    __builtin___clear_cache((char *)tramp, (char *)tramp + 16);

    t[0] = 0xe51ff004;                      /* ldr pc, [pc, #-4] */
    t[1] = (unsigned int)hookfn;
    __builtin___clear_cache((char *)target, (char *)target + 8);
    return tramp;
}
static struct SupportModulePriv wheresmywater_priv;

/* ---- coordinate transform: raw SDL pixels -> normalized 0..1, un-rotated ---- */
static void
wmw_normalize(struct SupportModulePriv *p, int rawx, int rawy, float *fx, float *fy)
{
    float W = (float)(p->width  ? p->width  : 1024);
    float H = (float)(p->height ? p->height :  768);
    *fx = (H - (float)rawy) / H;
    *fy = (float)rawx / W;
}

/* ---- build a JNI float/int array from a C array via the fake-JNI ---- */
static jfloatArray
wmw_make_float_array(JNIEnv *env, const float *vals, int n)
{
    jfloatArray a = (*env)->NewFloatArray(env, n);
    (*env)->SetFloatArrayRegion(env, a, 0, n, vals);
    return a;
}

static jintArray
wmw_make_int_array(JNIEnv *env, const jint *vals, int n)
{
    jintArray a = (*env)->NewIntArray(env, n);
    (*env)->SetIntArrayRegion(env, a, 0, n, vals);
    return a;
}

/* ---- dispatch helpers (handle both legacy and multitouch assembly) ---- */

/* Build the array of currently-active fingers. In legacy mode we always emit a
 * single touch with id 0 at the given coords. Returns the count. */
static int
wmw_collect(struct SupportModulePriv *p, int changed_finger,
            float *xs, float *ys, float *pxs, float *pys, jint *ids)
{
    int n = 0;
    if (!p->multitouch) {
        /* Legacy: single pinned finger (id 0) = the changed finger's coords. */
        WmwFinger *f = &p->fingers[changed_finger];
        xs[0] = f->x; ys[0] = f->y; pxs[0] = f->px; pys[0] = f->py; ids[0] = 0;
        return 1;
    }
    for (int i = 0; i < WMW_MAX_FINGERS; i++) {
        if (p->fingers[i].down) {
            xs[n] = p->fingers[i].x;
            ys[n] = p->fingers[i].y;
            pxs[n] = p->fingers[i].px;
            pys[n] = p->fingers[i].py;
            ids[n] = i;
            n++;
        }
    }
    return n;
}

static void
wmw_log_touch(struct SupportModulePriv *p, const char *what, int n,
              const float *xs, const float *ys, const jint *ids)
{
    fprintf(stderr, "[WMWTOUCH] f%d %s mode=%s n=%d", p->frame, what,
            p->multitouch ? "multi" : "legacy", n);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, " {id=%d %.4f,%.4f}", ids[i], xs[i], ys[i]);
    }
    fprintf(stderr, "\n");
}

/* The actual engine touch dispatch. Runs on the UI thread when enabled,
 * else inline on the main thread. Touches p->fingers[] from one thread only. */
static void
wmw_dispatch_touch(struct SupportModule *self, int event, int x, int y, int finger)
{
    struct SupportModulePriv *p = self->priv;
    JNIEnv *env = ENV_M;

    if (finger < 0 || finger >= WMW_MAX_FINGERS) finger = 0;
    WmwFinger *f = &p->fingers[finger];

    float fx, fy;
    wmw_normalize(p, x, y, &fx, &fy);

    float xs[WMW_MAX_FINGERS], ys[WMW_MAX_FINGERS];
    float pxs[WMW_MAX_FINGERS], pys[WMW_MAX_FINGERS];
    jint ids[WMW_MAX_FINGERS];
    int n;

    if (event == ACTION_DOWN) {
        f->down = 1;
        f->px = f->x = fx;
        f->py = f->y = fy;
        n = wmw_collect(p, finger, xs, ys, pxs, pys, ids);
        wmw_log_touch(p, "BEGAN", n, xs, ys, ids);
        jfloatArray jxs = wmw_make_float_array(env, xs, n);
        jfloatArray jys = wmw_make_float_array(env, ys, n);
        jintArray jids = wmw_make_int_array(env, ids, n);
        p->rendererTouchBegan(env, GLOBAL_M, n, jxs, jys, jids);
        if (p->double_down) {
            /* 2nd BEGAN: finds the FingerInfo created by the 1st -> hit-test -> catch */
            jfloatArray jxs2 = wmw_make_float_array(env, xs, n);
            jfloatArray jys2 = wmw_make_float_array(env, ys, n);
            jintArray jids2 = wmw_make_int_array(env, ids, n);
            p->rendererTouchBegan(env, GLOBAL_M, n, jxs2, jys2, jids2);
        }
    } else if (event == ACTION_MOVE) {
        f->px = f->x; f->py = f->y;
        f->x = fx;    f->y = fy;
        f->down = 1;
        n = wmw_collect(p, finger, xs, ys, pxs, pys, ids);
        wmw_log_touch(p, "MOVED", n, xs, ys, ids);
        jfloatArray jxs = wmw_make_float_array(env, xs, n);
        jfloatArray jys = wmw_make_float_array(env, ys, n);
        jfloatArray jpxs = wmw_make_float_array(env, pxs, n);
        jfloatArray jpys = wmw_make_float_array(env, pys, n);
        jintArray jids = wmw_make_int_array(env, ids, n);
        p->rendererTouchMoved(env, GLOBAL_M, n, jxs, jys, jpxs, jpys, jids);
    } else if (event == ACTION_UP) {
        f->px = f->x; f->py = f->y;
        f->x = fx;    f->y = fy;
        /* collect while this finger is still marked down so it's included */
        n = wmw_collect(p, finger, xs, ys, pxs, pys, ids);
        wmw_log_touch(p, "ENDED", n, xs, ys, ids);
        jfloatArray jxs = wmw_make_float_array(env, xs, n);
        jfloatArray jys = wmw_make_float_array(env, ys, n);
        jintArray jids = wmw_make_int_array(env, ids, n);
        p->rendererTouchEnded(env, GLOBAL_M, n, jxs, jys, jids);
        f->down = 0;
    }
}

/* ---- Frame-paced touch delivery (faithful Android input cadence) -----------
 * Confirmed root cause of in-level dead touch: a tap's DOWN and UP can land on
 * the SAME engine frame (observed: [WMWTOUCH] f1723 BEGAN + f1723 ENDED), so the
 * engine's per-frame WidgetManager::update (which captures the finger in its
 * "Phase 2" enter-detection) never runs between them. The widget never
 * presses-then-releases -> the level-1 tutorial overlay never dismisses and the
 * carve FingerCatcher never receives the finger. On Android a human tap spans
 * several vsync frames, so DOWN and UP are always on separate frames with the GL
 * thread's update() running between. We reproduce that contract by advancing at
 * most ONE transition per finger per engine frame: DOWN on frame N, MOVEs
 * coalesced on N+1.., UP only on a later frame -> guarantees >=1 update()
 * between DOWN and UP. General (not WMW-specific). */
typedef struct {
    int active;       /* a gesture is in progress for this finger */
    int down_sent;    /* DOWN dispatched (capture can happen from next update) */
    int up_pending;   /* UP received, deferred to a later frame */
    int move_pending; /* a newer position is waiting */
    int x, y;         /* latest raw coords */
} FingerPace;
static FingerPace g_pace_f[WMW_MAX_FINGERS];
static int g_pace = 1;   /* APKENV_WMW_PACE: frame-paced delivery (default ON) */

/* Called from input (records intent only; never dispatches to the engine). */
static void
wmw_pace_record(int event, int x, int y, int finger)
{
    if (finger < 0 || finger >= WMW_MAX_FINGERS) finger = 0;
    FingerPace *p = &g_pace_f[finger];
    if (event == ACTION_DOWN) {
        p->active = 1; p->down_sent = 0; p->up_pending = 0; p->move_pending = 0;
        p->x = x; p->y = y;
    } else if (event == ACTION_MOVE) {
        if (p->active) { p->x = x; p->y = y; p->move_pending = 1; }
    } else if (event == ACTION_UP) {
        if (p->active) { p->x = x; p->y = y; p->up_pending = 1; }
    }
}

/* Advance every finger by ONE transition. Call once per frame BEFORE drawFrame
 * so the dispatched touch is in the engine's queue when pumpEvents drains it. */
static void
wmw_pace_advance(struct SupportModule *self)
{
    for (int f = 0; f < WMW_MAX_FINGERS; f++) {
        FingerPace *p = &g_pace_f[f];
        if (!p->active) continue;
        if (!p->down_sent) {
            wmw_dispatch_touch(self, ACTION_DOWN, p->x, p->y, f);
            p->down_sent = 1;          /* let update() capture before any UP/MOVE */
        } else if (p->up_pending) {
            wmw_dispatch_touch(self, ACTION_UP, p->x, p->y, f);
            p->active = 0;
        } else if (p->move_pending) {
            wmw_dispatch_touch(self, ACTION_MOVE, p->x, p->y, f);
            p->move_pending = 0;
        }
    }
}

/* Dedicated "UI thread": drains the touch queue and calls rendererTouch* on a
 * thread distinct from the one running rendererDrawFrame (the "GL thread"). */
static void *
wmw_ui_thread(void *arg)
{
    struct SupportModule *self = arg;
    struct SupportModulePriv *p = self->priv;

    fprintf(stderr, "[WMWTHREAD] ui-thread started tid=%lu\n",
            (unsigned long)pthread_self());

    pthread_mutex_lock(&p->q_lock);
    while (p->ui_running) {
        while (p->ui_running && p->q_head == p->q_tail) {
            pthread_cond_wait(&p->q_cond, &p->q_lock);
        }
        if (!p->ui_running) break;
        TouchEvt e = p->q[p->q_tail];
        p->q_tail = (p->q_tail + 1) % WMW_TOUCHQ;
        pthread_mutex_unlock(&p->q_lock);

        wmw_dispatch_touch(self, e.event, e.x, e.y, e.finger);

        pthread_mutex_lock(&p->q_lock);
    }
    pthread_mutex_unlock(&p->q_lock);
    return NULL;
}

static void
wheresmywater_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    struct SupportModulePriv *p = self->priv;

    if (g_pace) {
        /* record only; wmw_pace_advance() dispatches one transition per frame */
        wmw_pace_record(event, x, y, finger);
        return;
    }

    if (!p->use_uithread) {
        wmw_dispatch_touch(self, event, x, y, finger);
        return;
    }

    /* enqueue raw event for the UI thread; drop if the queue is full */
    pthread_mutex_lock(&p->q_lock);
    int nh = (p->q_head + 1) % WMW_TOUCHQ;
    if (nh != p->q_tail) {
        p->q[p->q_head].event = event;
        p->q[p->q_head].x = x;
        p->q[p->q_head].y = y;
        p->q[p->q_head].finger = finger;
        p->q_head = nh;
    }
    pthread_cond_signal(&p->q_cond);
    pthread_mutex_unlock(&p->q_lock);
}

static int
wheresmywater_try_init(struct SupportModule *self)
{
    self->priv->JNI_OnLoad = (jni_onload_t)LOOKUP_M("JNI_OnLoad");
    self->priv->rendererInit = (wmw_init_t)LOOKUP_M("WMWRenderer_rendererInit");
    self->priv->rendererResized = (wmw_resized_t)LOOKUP_M("WMWRenderer_rendererResized");
    self->priv->notifyScreenResized = (wmw_resized_t)LOOKUP_M("WMWRenderer_notifyScreenResized");
    self->priv->rendererDrawFrame = (wmw_drawframe_t)LOOKUP_M("WMWRenderer_rendererDrawFrame");
    self->priv->rendererReloadContextData = (wmw_reload_t)LOOKUP_M("WMWRenderer_rendererReloadContextData");
    self->priv->rendererTouchBegan = (wmw_touch3_t)LOOKUP_M("WMWRenderer_rendererTouchBegan");
    self->priv->rendererTouchEnded = (wmw_touch3_t)LOOKUP_M("WMWRenderer_rendererTouchEnded");
    self->priv->rendererTouchMoved = (wmw_touchmoved_t)LOOKUP_M("WMWRenderer_rendererTouchMoved");
    self->priv->onGamePause = (wmw_void_t)LOOKUP_M("BaseActivity_onGamePause");
    self->priv->onGameResume = (wmw_void_t)LOOKUP_M("BaseActivity_onGameResume");
    self->priv->onLostFocus = (wmw_void_t)LOOKUP_M("BaseActivity_onLostFocus");
    self->priv->onRegainedFocus = (wmw_void_t)LOOKUP_M("BaseActivity_onRegainedFocus");
    self->priv->accelerometerChanged = (wmw_accel_t)LOOKUP_M("BaseActivity_accelerometerChanged");
    self->priv->backKeyPressed = (wmw_void_t)LOOKUP_M("BaseActivity_backKeyPressed");

    self->priv->global = GLOBAL_M;

    return (self->priv->JNI_OnLoad != NULL &&
            self->priv->rendererInit != NULL &&
            self->priv->rendererResized != NULL &&
            self->priv->rendererDrawFrame != NULL &&
            self->priv->rendererTouchBegan != NULL &&
            self->priv->rendererTouchMoved != NULL &&
            self->priv->rendererTouchEnded != NULL);
}

static void
wheresmywater_init(struct SupportModule *self, int width, int height, const char *home)
{
    struct SupportModulePriv *p = self->priv;
    JNIEnv *env = ENV_M;

    p->width = width;
    p->height = height;
    p->frame = 0;
    memset(p->fingers, 0, sizeof(p->fingers));

    const char *mt = getenv("APKENV_WMW_MULTITOUCH");
    p->multitouch = (mt && mt[0] == '1') ? 1 : 0;
    fprintf(stderr, "wheresmywater_init: %dx%d home=%s touch-mode=%s\n",
            width, height, home, p->multitouch ? "MULTI" : "legacy");

    /* Portrait render. Two strategies (A/B via APKENV_WMW_FBO):
     *
     *  FBO (Stage 3, APKENV_WMW_FBO=1): the engine renders its whole frame
     *    un-rotated into an offscreen 768x1024 portrait FBO; the platform
     *    rotates it ONCE at present (apkenv_fbo_present). Keep
     *    current_orientation == platform (LANDSCAPE) so the per-call rotation
     *    hooks stay OFF. This makes the engine's entire internal frame
     *    self-consistent portrait (target fix for the water-sim orientation).
     *
     *  legacy (default): apkenv's per-call GLES1 fixed-function rotation hooks
     *    (the current working path for world/UI; water renders rotated). */
    {
        /* FBO render is the correct path (fixes water + orientation); default ON,
         * disable with APKENV_WMW_FBO=0 to fall back to the legacy per-call hooks. */
        const char *use_fbo = getenv("APKENV_WMW_FBO");
        if (!use_fbo || use_fbo[0] != '0') {
            GLOBAL_M->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;
            GLOBAL_M->module_hacks->gles_viewport_hack = 0;
            GLOBAL_M->module_hacks->glOrthof_rotation_hack = 0;
            GLOBAL_M->module_hacks->render_to_fbo = 1;
            GLOBAL_M->module_hacks->fbo_w = 768;
            GLOBAL_M->module_hacks->fbo_h = 1024;
            fprintf(stderr, "wheresmywater_init: RENDER-TO-PORTRAIT-FBO mode (Stage 3)\n");
        } else {
            GLOBAL_M->module_hacks->current_orientation = ORIENTATION_PORTRAIT;
            GLOBAL_M->module_hacks->gles_viewport_hack = 1;
            GLOBAL_M->module_hacks->glOrthof_rotation_hack = 1;
        }
    }

    p->JNI_OnLoad(VM_M, NULL);

    /* Compute load slide NOW (rendererTouchBegan already resolved) so we can set
     * GameSettings::CurrentDeviceOrientation BEFORE the world/camera is built in
     * rendererInit (a per-frame write is too late — the camera caches it). */
    if (p->rendererTouchBegan) {
        p->slide = (unsigned long)p->rendererTouchBegan - WMW_LINK_rendererTouchBegan;
        g_slide = p->slide;
    }
    {
        const char *o = getenv("APKENV_WMW_ORIENT");
        if (o) g_orient = atoi(o);
        if (g_orient >= 0 && p->slide) {
            *(int *)(WMW_LINK_CurrentDeviceOrientation + p->slide) = g_orient;
            fprintf(stderr, "wheresmywater_init: CurrentDeviceOrientation=%d set BEFORE rendererInit\n", g_orient);
        }
    }

    /* sourceDir = the apk path (engine's ZipArchive reads assets from it);
     * dataDir = a writable home dir; context = dummy non-null jobject. */
    jstring sourceDir = (*env)->NewStringUTF(env, GLOBAL_M->apk_filename);
    jstring dataDir = (*env)->NewStringUTF(env, home);
    p->rendererInit(env, GLOBAL_M, sourceDir, dataDir, (jobject)GLOBAL_M);

    p->rendererResized(env, GLOBAL_M, width, height);
    if (p->notifyScreenResized)
        p->notifyScreenResized(env, GLOBAL_M, width, height);

    if (p->onGameResume) p->onGameResume(env, GLOBAL_M);
    if (p->onRegainedFocus) p->onRegainedFocus(env, GLOBAL_M);

    /* Start the UI thread AFTER the engine is initialized (so rendererTouch*
     * is valid). Default ON; APKENV_WMW_UITHREAD=0 reverts to inline dispatch
     * on the main thread (the old single-thread behavior). */
    const char *pc = getenv("APKENV_WMW_PACE");
    g_pace = (pc && pc[0] == '0') ? 0 : 1;   /* frame-paced delivery, default ON */
    memset(g_pace_f, 0, sizeof(g_pace_f));
    /* Neutralize the full-screen "press Swampy" PushButton that swallows all
     * in-level touch; default ON (carve + HUD work). Disable with =0. */
    const char *kt = getenv("APKENV_WMW_KILLTHIEF");
    g_killthief = (kt && kt[0] == '0') ? 0 : 1;
    const char *ut = getenv("APKENV_WMW_UITHREAD");
    /* paced delivery runs on the GL thread (no separate UI thread needed) */
    p->use_uithread = (g_pace || (ut && ut[0] == '0')) ? 0 : 1;
    fprintf(stderr, "wheresmywater_init: main(GL) tid=%lu touch-dispatch=%s\n",
            (unsigned long)pthread_self(),
            p->use_uithread ? "UI-THREAD" : "inline");
    /* Compute the runtime address of ScreenManager::mCurrentTransition from
     * the load slide (rendererTouchBegan is already resolved). */
    if (p->rendererTouchBegan) {
        p->slide = (unsigned long)p->rendererTouchBegan - WMW_LINK_rendererTouchBegan;
        g_slide = p->slide;
        p->mtrans = (void **)(WMW_LINK_mCurrentTransition + p->slide);
        p->sstack = (char ***)(WMW_LINK_mScreenStack + p->slide);
        p->wt_vtable = WMW_LINK_WT_vtableptr + p->slide;
        const char *fi = getenv("APKENV_WMW_FORCEINPUT");
        p->force_input = (fi && fi[0] == '1') ? 1 : 0;
        const char *cr = getenv("APKENV_WMW_CALLREGAIN");
        p->call_regain = (cr && cr[0] == '1') ? 1 : 0;
        const char *dd = getenv("APKENV_WMW_DOUBLEDOWN");
        p->double_down = (dd && dd[0] == '1') ? 1 : 0;
        const char *ec = getenv("APKENV_WMW_ENTERCATCH");
        g_enter_catch = (ec && ec[0] == '1') ? 1 : 0;
        const char *fc = getenv("APKENV_WMW_FORCECATCH");
        g_force_catch = (fc && fc[0] == '1') ? 1 : 0;
        const char *rh = getenv("APKENV_WMW_REHIT");
        g_rehit = (rh && rh[0] == '1') ? 1 : 0;
        const char *rh2 = getenv("APKENV_WMW_REHIT2");
        g_rehit2 = (rh2 && rh2[0] == '1') ? 1 : 0;
        const char *fa = getenv("APKENV_WMW_ACCEL");
        g_feed_accel = (fa && fa[0] == '0') ? 0 : 1;   /* default ON */
        const char *gx = getenv("APKENV_WMW_GX"); if (gx) g_gx = (float)atof(gx);
        const char *gy = getenv("APKENV_WMW_GY"); if (gy) g_gy = (float)atof(gy);
        const char *orient = getenv("APKENV_WMW_ORIENT"); if (orient) g_orient = atoi(orient);
        fprintf(stderr, "wheresmywater_init: feed_accel=%d gravity=(%.2f,%.2f,0)\n",
                g_feed_accel, g_gx, g_gy);
        g_acceptFinger = (void *)(WMW_LINK_FC_acceptFinger + p->slide);
        p->wt_regainedTop = (void *)(WMW_LINK_WT_regainedTop + p->slide);
        fprintf(stderr, "wheresmywater_init: slide=0x%lx mCurrentTransition@%p "
                "mScreenStack@%p force_input=%d\n", p->slide, (void *)p->mtrans,
                (void *)p->sstack, p->force_input);

        /* inline-hook Screen_WaterTest::touch{Down,Moved} to confirm they run */
        const char *hk = getenv("APKENV_WMW_HOOK");
        if (!hk || hk[0] != '0') {
            wt_orig_touchDown = (void *)wmw_install_hook(
                WMW_LINK_WT_touchDown + p->slide, (void *)wt_hook_touchDown);
            wt_orig_touchMoved = (void *)wmw_install_hook(
                WMW_LINK_WT_touchMoved + p->slide, (void *)wt_hook_touchMoved);
            wt_orig_touchUp = (void *)wmw_install_hook(
                WMW_LINK_WT_touchUp + p->slide, (void *)wt_hook_touchUp);
            aabb_orig_contains = (void *)wmw_install_hook(
                WMW_LINK_AABB_contains + p->slide, (void *)aabb_hook_contains);
            world_orig_touchDown = (void *)wmw_install_hook(
                WMW_LINK_World_touchDown + p->slide, (void *)world_hook_touchDown);
            world_orig_touchMoved = (void *)wmw_install_hook(
                WMW_LINK_World_touchMoved + p->slide, (void *)world_hook_touchMoved);
            world_orig_touchUp = (void *)wmw_install_hook(
                WMW_LINK_World_touchUp + p->slide, (void *)world_hook_touchUp);
            fc_orig_acceptFinger = (void *)wmw_install_hook(
                WMW_LINK_FC_acceptFinger + p->slide, (void *)fc_hook_acceptFinger);
            fc_orig_acceptEntered = (void *)wmw_install_hook(
                WMW_LINK_FC_acceptEntered + p->slide, (void *)fc_hook_acceptEntered);
            wm_orig_touchDown = (void *)wmw_install_hook(
                WMW_LINK_WM_touchDown + p->slide, (void *)wm_hook_touchDown);
            wm_orig_update = (void *)wmw_install_hook(
                WMW_LINK_WM_update + p->slide, (void *)wm_hook_update);
            wm_orig_addWidget = (void *)wmw_install_hook(
                WMW_LINK_WM_addWidget + p->slide, (void *)wm_hook_addWidget);
            wt_orig_handleEvent = (void *)wmw_install_hook(
                WMW_LINK_WT_handleEvent + p->slide, (void *)wt_hook_handleEvent);
            fprintf(stderr, "[WMWHOOK] installed: touchDown=%p touchMoved=%p "
                    "touchUp=%p contains=%p worldDown=%p worldMoved=%p handleEvent=%p\n",
                    (void *)wt_orig_touchDown, (void *)wt_orig_touchMoved,
                    (void *)wt_orig_touchUp, (void *)aabb_orig_contains,
                    (void *)world_orig_touchDown, (void *)world_orig_touchMoved,
                    (void *)wt_orig_handleEvent);
        }
    }

    if (p->use_uithread) {
        p->q_head = p->q_tail = 0;
        p->ui_running = 1;
        pthread_mutex_init(&p->q_lock, NULL);
        pthread_cond_init(&p->q_cond, NULL);
        if (pthread_create(&p->ui_thread, NULL, wmw_ui_thread, self) != 0) {
            fprintf(stderr, "wheresmywater_init: ui-thread create FAILED, "
                    "falling back to inline\n");
            p->use_uithread = 0;
        }
    }

    /* Audio: FMOD::System::init() ran inside rendererInit, so the device pump
     * can now poll fmodGetInfo for the mix format. On Android WMWActivity
     * starts FMODAudioDevice in onCreate/onResume — we do the equivalent. */
    const char *au = getenv("APKENV_WMW_AUDIO");
    g_audio = (au && au[0] == '0') ? 0 : 1;
    if (g_audio) apkenv_fmod_pump_start(GLOBAL_M);
}

static void
wheresmywater_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    /* AKEYCODE_BACK == 4 */
    if (event == ACTION_DOWN && keycode == 4 && self->priv->backKeyPressed) {
        self->priv->backKeyPressed(ENV_M, GLOBAL_M);
    }
}

static void
wheresmywater_update(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    p->frame++;

    /* Frame-paced touch: dispatch at most one transition per finger this frame,
     * BEFORE rendererDrawFrame, so the engine's pumpEvents + WidgetManager::update
     * run between a tap's DOWN and UP (the in-level capture cadence). */
    if (g_pace) wmw_pace_advance(self);

    /* Override the game's device-orientation each frame (controls world camera
     * rotation AND gravity sign). Test values 0-3 via APKENV_WMW_ORIENT. */
    if (g_orient >= 0 && p->slide) {
        *(int *)(WMW_LINK_CurrentDeviceOrientation + p->slide) = g_orient;
    }

    /* Feed the accelerometer (gravity) every frame — we previously never called
     * this, so WMW's gravity sat at a default (water flowed up). Fixed
     * upright-portrait vector for now (env-tunable sign via APKENV_WMW_GX/GY). */
    if (g_feed_accel && p->accelerometerChanged) {
        p->accelerometerChanged(ENV_M, GLOBAL_M, g_gx, g_gy, 0.0f);
    }

    /* per-second probe of the touch gate + active screen stack */
    if (p->mtrans && (p->frame % 60 == 0)) {
        fprintf(stderr, "[WMWGATE] frame %d mCurrentTransition=%p (%s)\n",
                p->frame, *p->mtrans,
                *p->mtrans ? "TOUCH GATED OFF" : "touch passes");
    }
    /* Resolve the active top screen; if it's Screen_WaterTest, inspect/force
     * its WidgetManager gate (wm = screen+0x50, gate byte = wm+0x60). */
    char *wm = NULL;
    if (p->sstack) {
        char **start = p->sstack[0];
        char **finish = p->sstack[1];
        int n = (start && finish) ? (int)(finish - start) : -1;
        char *top = (n > 0) ? finish[-1] : NULL;
        unsigned long topvt = top ? (*(unsigned long *)top) : 0;

        g_in_level = (top && topvt == p->wt_vtable) ? 1 : 0;
        if (top && topvt == p->wt_vtable) {
            wm = *(char **)(top + 0x50);
            /* force the manager-level input gate on every frame (experiment) */
            if (p->force_input && wm) *(unsigned char *)(wm + 0x60) = 1;

            /* When a finger (id 0) is in the WidgetManager map but uncaptured,
             * trigger a real capture once per finger-down:
             *   REHIT (general; fixes buttons AND carve): re-invoke the engine's
             *     WidgetManager::touchDown so it FINDS the now-existing FingerInfo
             *     and runs the hit-test, capturing whatever widget is under the
             *     touch (button or FingerCatcher) via the engine's own code.
             *   FORCECATCH (carve only, fallback): call the FingerCatcher's
             *     _acceptFinger directly. */
            if ((g_rehit || g_force_catch) && wm) {
                char *node = *(char **)(wm + 0x2c);   /* finger-map root */
                char *fi = NULL; int guard = 0;
                while (node && guard++ < 64) {
                    int key = *(int *)(node + 16);
                    if (0 < key)      node = *(char **)(node + 8);
                    else if (0 > key) node = *(char **)(node + 12);
                    else { fi = *(char **)(node + 20); break; }
                }
                if (fi && !g_forced) {
                    /* systematic: dump the FingerInfo state the engine sees, to
                     * understand why its own catch (enter-detection) skips it. */
                    unsigned int *F = (unsigned int *)fi;
                    fprintf(stderr, "[WMWFI] fi=%p w0=%08x x=%08x y=%08x w12=%08x "
                            "w16=%08x cap20=%08x w24=%08x w28=%08x\n", (void *)fi,
                            F[0], F[1], F[2], F[3], F[4], F[5], F[6], F[7]);
                    g_forced = 1;
                    if (g_rehit && wm_orig_touchDown) {
                        /* FingerInfo pos is at fi+4 (x) / fi+8 (y) = a Vector2 */
                        fprintf(stderr, "[WMWREHIT] re-touchDown(wm=%p,0,pos@%p)\n",
                                (void *)wm, (void *)(fi + 4));
                        wm_orig_touchDown(wm, 0, (V2 *)(fi + 4));
                    } else if (g_force_catch && g_catcher && g_acceptFinger) {
                        fprintf(stderr, "[WMWFORCE] _acceptFinger(catcher=%p,0,fi=%p)\n",
                                g_catcher, (void *)fi);
                        g_acceptFinger(g_catcher, 0, fi);
                    }
                } else if (!fi) {
                    g_forced = 0;   /* finger lifted; arm for next down */
                }
            }

            /* experiment: ~2s after entering the level, call regainedTop()
             * (the post-pause path that enables the interactive widgets). */
            p->wt_active_frames++;
            if (p->call_regain && !p->regain_done && p->wt_active_frames == 120) {
                fprintf(stderr, "[WMWREGAIN] calling Screen_WaterTest::regainedTop(%p)\n",
                        (void *)top);
                p->wt_regainedTop(top);
                p->regain_done = 1;
                fprintf(stderr, "[WMWREGAIN] returned\n");
            }
        } else {
            p->wt_active_frames = 0;
        }

        if (p->frame % 60 == 0) {
            int gate = wm ? *(unsigned char *)(wm + 0x60) : -1;
            fprintf(stderr, "[WMWSTACK] frame %d screens=%d top=%p "
                    "top_vtable_link=0x%lx %s wm=%p gate=%d%s\n",
                    p->frame, n, (void *)top, topvt ? (topvt - p->slide) : 0,
                    (topvt == p->wt_vtable) ? "WATERTEST" : "",
                    (void *)wm, gate, p->force_input ? " (FORCED)" : "");
        }
    }

    p->rendererDrawFrame(ENV_M, GLOBAL_M);
}

static void
wheresmywater_deinit(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    if (g_audio) apkenv_fmod_pump_stop();
    if (p->use_uithread) {
        pthread_mutex_lock(&p->q_lock);
        p->ui_running = 0;
        pthread_cond_signal(&p->q_cond);
        pthread_mutex_unlock(&p->q_lock);
        pthread_join(p->ui_thread, NULL);
    }
}

static void
wheresmywater_pause(struct SupportModule *self)
{
    fprintf(stderr, "[WMWLIFE] pause (onGamePause + onLostFocus) @frame %d\n",
            self->priv->frame);
    if (g_audio) apkenv_fmod_pump_stop();
    if (self->priv->onGamePause) self->priv->onGamePause(ENV_M, GLOBAL_M);
    if (self->priv->onLostFocus) self->priv->onLostFocus(ENV_M, GLOBAL_M);
}

static void
wheresmywater_resume(struct SupportModule *self)
{
    fprintf(stderr, "[WMWLIFE] resume (onGameResume + onRegainedFocus) @frame %d\n",
            self->priv->frame);
    if (self->priv->onGameResume) self->priv->onGameResume(ENV_M, GLOBAL_M);
    if (self->priv->onRegainedFocus) self->priv->onRegainedFocus(ENV_M, GLOBAL_M);
    if (g_audio) apkenv_fmod_pump_start(GLOBAL_M);
}

static int
wheresmywater_requests_exit(struct SupportModule *self)
{
    return 0;
}

APKENV_MODULE(wheresmywater, MODULE_PRIORITY_GAME_VERSION)
