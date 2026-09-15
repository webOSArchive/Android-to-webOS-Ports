/**
 * apkenv — EA **BLAST** engine support module.
 *
 * Named for the engine, not the game: `com.ea.blast` + EAIO / EAThread /
 * EAAudioCore / rwfilesystem is EA Mobile's shared Android host from the
 * 2011-2013 era, so this should carry to their other ports. First target is
 * Dead Space (`com.eamobile.deadspace_full_azn` 1.2.0).
 *
 * Contract derived statically before any device run; the evidence, the ranked
 * risks and the test protocol are in plan/DEAD-SPACE.md, the full surface in
 * plan/deadspace-contract.txt.
 *
 * What is different about this host, and worth not getting wrong:
 *
 *  1. **The engine publishes its own constants.** Java does not hardcode
 *     Android's MotionEvent actions — it asks the engine
 *     (NativeGetIdRawPointerDown/Up/Move/Cancel, NativeGetModuleTypeId*) at
 *     class-init and passes those values back. So this module must ASK, not
 *     invent. Getting that wrong is invisible: the engine would just ignore
 *     every pointer event.
 *
 *  2. **Touch coordinates are PIXELS.** TouchSurfaceAndroid.SendRawPointerEvent
 *     calls getX(i)/getY(i) with no division — the opposite of Where's My Water
 *     and Fruit Ninja, both of which normalise. Read the caller every time.
 *
 *  3. **Pure GLES1 fixed-function.** 190 gl* imports, all ES1 + OES; no shader
 *     calls, no eglGetProcAddress, DT_NEEDED is libGLESv1_CM alone — despite
 *     the manifest declaring uses-gl-es 0x20000. Trust the binary.
 *
 *  4. **NativeOnPause comes BEFORE the GL thread stops** here
 *     (MainActivity.onPause: NativeOnPause() then mGLView.onPause()), which is
 *     the reverse of Mortar's order. Fruit Ninja crashes on pause and that
 *     ordering is the prime suspect there, so encode this one from the caller
 *     rather than copying the last module.
 */

#include "common.h"
#include "../audio/audiotrack.h"

#include <limits.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── engine entry points (nm -D libDeadSpace.so | grep Java_) ──────────────── */
typedef void (*blast_void_t)(JNIEnv *, jobject) SOFTFP;
typedef jint (*blast_int_t)(JNIEnv *, jobject) SOFTFP;
typedef void (*blast_bool_arg_t)(JNIEnv *, jobject, jboolean) SOFTFP;
typedef void (*blast_surfchanged_t)(JNIEnv *, jobject, jint, jint) SOFTFP;
typedef void (*blast_pointer_t)(JNIEnv *, jclass, jint, jint, jint, jfloat, jfloat) SOFTFP;
typedef void (*blast_startup_am_t)(JNIEnv *, jclass, jobject) SOFTFP;
typedef void (*blast_audioinit_t)(JNIEnv *, jclass, jobject, jint, jint, jint) SOFTFP;
typedef void (*blast_accel_t)(JNIEnv *, jobject, jfloat, jfloat, jfloat) SOFTFP;

struct SupportModulePriv {
    jni_onload_t        JNI_OnLoad;

    /* lifecycle (MainActivity) */
    blast_void_t        NativeOnCreate;
    blast_void_t        NativeOnPause;
    blast_void_t        NativeOnResume;
    blast_void_t        NativeOnStop;
    blast_void_t        NativeOnLowMemory;
    blast_void_t        NativeOsExit;
    blast_int_t         NativeGetExitCode;
    blast_bool_arg_t    NativeOnWindowFocusChanged;

    /* renderer (AndroidRenderer) */
    blast_void_t        NativeOnSurfaceCreated;
    blast_surfchanged_t NativeOnSurfaceChanged;
    blast_void_t        NativeOnDrawFrame;

    /* input (TouchSurfaceAndroid) */
    blast_pointer_t     NativeOnPointerEvent;
    blast_int_t         GetIdRawPointerDown;
    blast_int_t         GetIdRawPointerUp;
    blast_int_t         GetIdRawPointerMove;
    blast_int_t         GetIdRawPointerCancel;
    blast_int_t         GetIdUndefined;
    blast_int_t         GetModuleTypeIdTouchScreen;

    /* subsystems */
    blast_void_t        EAThread_Init;
    blast_startup_am_t  EAIO_Startup;
    blast_startup_am_t  rwfilesystem_Startup;
    blast_audioinit_t   AudioCore_Init;
    blast_accel_t       NativeOnAcceleration;

    char home[PATH_MAX];
    char content_root[PATH_MAX];
    int  screen_w, screen_h;
    int  want_exit;

    /* engine-supplied constants, fetched once in init() */
    jint id_down, id_up, id_move, id_cancel, id_undefined, module_touchscreen;
};
static struct SupportModulePriv eablast_priv;
static struct GlobalState *global;

#define method_is(m) (0 == strcmp(method->name, #m))

/* ── engine->host call-out tracer (PORTING-PLAYBOOK.md §3) ─────────────────── */
#define BLAST_TRACE_MAX 96
static struct { char *name; unsigned long n; } blast_trace[BLAST_TRACE_MAX];
static int blast_trace_n = 0;
static unsigned long blast_frames = 0;

static void
blast_trace_unhandled(const char *kind, jmethodID method)
{
    int i;
    for (i = 0; i < blast_trace_n; i++)
        if (strcmp(blast_trace[i].name, method->name) == 0) {
            unsigned long n = ++blast_trace[i].n;
            if (n == 100 || n == 10000 || n == 1000000)
                fprintf(stderr, "[BLAST-JNI] %s %s called %lu times (frame=%lu)\n",
                        kind, method->name, n, blast_frames);
            return;
        }
    fprintf(stderr, "[BLAST-JNI] UNHANDLED %s %s%s (frame=%lu)\n",
            kind, method->name, method->sig ? method->sig : "", blast_frames);
    if (blast_trace_n < BLAST_TRACE_MAX) {
        blast_trace[blast_trace_n].name = strdup(method->name);
        blast_trace[blast_trace_n].n = 1;
        blast_trace_n++;
    }
}

/* ── audio ─────────────────────────────────────────────────────────────────
 * AndroidEAAudioCore.Startup() builds an AudioTrack(STREAM_MUSIC, rate,
 * CHANNEL_OUT_STEREO, PCM_16BIT, minBuf, MODE_STREAM) and hands the OBJECT to
 * native Init(track, rate, channels, minBuf); the engine then calls write() on
 * it over JNI. So we stand in for the AudioTrack: our object, its write routed
 * into audio/audiotrack.c. Java takes the rate from
 * AudioTrack.getNativeOutputSampleRate(), i.e. the device decides — 44100 here. */
#define BLAST_AUDIO_RATE     44100
#define BLAST_AUDIO_CHANNELS 2
#define BLAST_AUDIO_BUFBYTES 8192

/* Layout-compatible with struct dummy_jclass (name first) so GetObjectClass can
 * name it rather than hand back a sentinel. */
struct blast_object { char *name; };
static struct blast_object audio_track_obj = { (char *)"android/media/AudioTrack" };
static struct dummy_jclass asset_manager_class = { (char *)"android/content/res/AssetManager" };

static AudioTrack *audio_track = NULL;
static unsigned long audio_writes = 0, audio_bytes = 0;

static void
blast_audio_open(void)
{
    if (audio_track != NULL)
        return;
    audio_track = apkenv_audiotrack_create(BLAST_AUDIO_RATE, BLAST_AUDIO_CHANNELS,
                                           BLAST_AUDIO_BUFBYTES);
    if (audio_track == NULL) {
        fprintf(stderr, "[BLAST-AUDIO] AudioTrack create FAILED — the game will be silent\n");
        return;
    }
    fprintf(stderr, "[BLAST-AUDIO] AudioTrack %d/%d open\n",
            BLAST_AUDIO_RATE, BLAST_AUDIO_CHANNELS);
    apkenv_audiotrack_play(audio_track);
}

static void
blast_audio_write(const void *data, int bytes)
{
    if (audio_track == NULL || bytes <= 0)
        return;
    apkenv_audiotrack_write(audio_track, data, bytes);
    audio_bytes += (unsigned long)bytes;
    audio_writes++;
    /* Periodic meter, not a one-shot line: bytes/second against
     * rate*channels*2 says the pump tracks real time, and an underrun delta of
     * 0 says the ring never ran dry (PORTING-PLAYBOOK.md §4). */
    {
        static unsigned long next_report = 0, last_underrun = 0;
        unsigned long secs = audio_bytes / (BLAST_AUDIO_RATE * BLAST_AUDIO_CHANNELS * 2);
        if (audio_writes == 1 || secs >= next_report) {
            unsigned long u = apkenv_audiotrack_underrun_bytes(audio_track);
            fprintf(stderr, "[BLAST-AUDIO] %lu writes, %lu bytes (%lus of audio), underrun +%lu\n",
                    audio_writes, audio_bytes, secs, u - last_underrun);
            last_underrun = u;
            next_report = secs + 10;
        }
    }
}

/* ── fake-JNI overrides ─────────────────────────────────────────────────────
 * Every ...V form the engine can reach is overridden: a missing one falls
 * through to jni/jnienv.c, whose CallObjectMethodV returns the GLOBAL_J
 * sentinel and turns an unanswered host call into a SIGSEGV in strlen
 * (PORTING-PLAYBOOK.md §4). */

static jclass
blast_FindClass(JNIEnv *env, const char *name)
{
    struct dummy_jclass *c = malloc(sizeof(*c));
    c->name = strdup(name);
    return c;
}

static jclass
blast_GetObjectClass(JNIEnv *env, jobject obj)
{
    struct dummy_jclass *c = malloc(sizeof(*c));
    c->name = strdup(obj == (jobject)&audio_track_obj
                     ? audio_track_obj.name : "java/lang/Object");
    return c;
}

static jthrowable blast_ExceptionOccurred(JNIEnv *env) { return NULL; }
static void       blast_ExceptionClear(JNIEnv *env) { }
static jboolean   blast_ExceptionCheck(JNIEnv *env) { return JNI_FALSE; }

/* SystemAndroidDelegate returns every device fact as a String, counts
 * included. These are read once at startup and believed; GetTotalRAM in
 * particular is likely to size caches on a game this big, so it is stated
 * honestly rather than inflated. */
static jobject
blast_CallStaticObjectMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    const char *v = NULL;

    if (method_is(GetAppDataDirectory))        v = eablast_priv.home;
    else if (method_is(GetExternalStorageDirectory)) v = eablast_priv.content_root;
    else if (method_is(GetTotalRAM))           v = getenv("APKENV_BLAST_TOTALRAM")
                                                   ? getenv("APKENV_BLAST_TOTALRAM") : "536870912";
    else if (method_is(GetDeviceModel))        v = "TouchPad";
    else if (method_is(GetDeviceName))         v = "TouchPad";
    else if (method_is(GetManufacturer))       v = "HP";
    else if (method_is(GetChipset))            v = "APQ8060";
    else if (method_is(GetProcessorArchitecture)) v = "armeabi";
    else if (method_is(GetApiLevel))           v = "10";
    else if (method_is(GetPlatformStdName))    v = "Android";
    else if (method_is(GetPlatformRawName))    v = "webos";
    else if (method_is(GetPlatformVersion))    v = "2.3.6";
    else if (method_is(GetLanguage))           v = "en";
    else if (method_is(GetLocale))             v = "en_US";
    else if (method_is(GetDeviceUniqueId))     v = "apkenvwebos000000";
    else if (method_is(GetDeviceSubscriberID)) v = "";
    else if (method_is(GetPhoneNumber))        v = "";
    /* Hardware the TouchPad has, as counts-in-strings. */
    else if (method_is(GetDisplayCount))       v = "1";
    else if (method_is(GetTouchScreenCount))   v = "1";
    else if (method_is(GetAccelerometerCount)) v = "1";
    else if (method_is(GetVibratorCount))      v = "1";
    /* ...and what it does not. */
    else if (method_is(GetCompassCount) || method_is(GetGyroscopeCount) ||
             method_is(GetCameraCount) || method_is(GetMicrophoneCount) ||
             method_is(GetTouchPadCount) || method_is(GetTrackBallCount) ||
             method_is(GetPhysicalKeyboardCount) || method_is(GetVirtualKeyboardCount))
        v = "0";
    else if (method_is(GetLocationAvailable) || method_is(IsBatteryStateAvailable))
        v = "false";

    if (v != NULL) {
        static int logged = 0;
        if (logged++ < 40)
            fprintf(stderr, "[BLAST-SYS] %s -> \"%s\"\n", method->name, v);
        return (*env)->NewStringUTF(env, v);
    }

    blast_trace_unhandled("obj", method);
    return NULL;
}

static jobject
blast_CallObjectMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    /* The delegates are instance methods on objects the engine holds. */
    return blast_CallStaticObjectMethodV(env, (jclass)obj, method, args);
}

static jint
blast_CallIntMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    if (method_is(GetDefaultWidth))  return eablast_priv.screen_w;
    if (method_is(GetDefaultHeight)) return eablast_priv.screen_h;
    if (method_is(GetStdOrientation)) return 0;   /* see NativeGetOrientationNormal */

    /* android.media.AudioTrack.write([BII)I / ([SII)I — the engine's audio sink. */
    if (method_is(write) && obj == (jobject)&audio_track_obj) {
        struct dummy_array *a = va_arg(args, struct dummy_array *);
        jint off = va_arg(args, jint);
        jint cnt = va_arg(args, jint);
        if (a && a->data && cnt > 0) {
            int esz = (int)a->element_size;
            blast_audio_write((const char *)a->data + (size_t)off * esz, cnt * esz);
        }
        return cnt;
    }

    blast_trace_unhandled("int", method);
    return 0;
}

static jint
blast_CallStaticIntMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    return blast_CallIntMethodV(env, (jobject)clazz, method, args);
}

static jfloat
blast_CallFloatMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    /* TouchPad: 1024x768 on 9.7" ≈ 132 dpi. */
    if (method_is(GetDpiX) || method_is(GetDpiY)) return 132.0f;
    blast_trace_unhandled("float", method);
    return 0.0f;
}

static jboolean
blast_CallBooleanMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    if (method_is(IsTouchScreenMultiTouch)) return JNI_TRUE;
    if (method_is(IntentView)) return JNI_FALSE;   /* no browser to hand off to */
    blast_trace_unhandled("bool", method);
    return JNI_FALSE;
}

static jboolean
blast_CallStaticBooleanMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    return blast_CallBooleanMethodV(env, (jobject)clazz, method, args);
}

static void
blast_CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    if (obj == (jobject)&audio_track_obj) {
        /* play/stop/pause/flush/release on our sink. */
        if (method_is(play))    { apkenv_audiotrack_play(audio_track);  return; }
        if (method_is(pause))   { apkenv_audiotrack_pause(audio_track); return; }
        if (method_is(stop))    { apkenv_audiotrack_stop(audio_track);  return; }
        if (method_is(flush))   { return; }
        if (method_is(release)) { return; }
    }
    if (method_is(SetStdOrientation)) {
        fprintf(stderr, "[BLAST] SetStdOrientation(%d)\n", va_arg(args, int));
        return;
    }
    if (method_is(SetEnabled))  return;   /* LocationManagerAndroid */
    if (method_is(Shutdown))    return;   /* VirtualKeyboardAndroidDelegate */
    blast_trace_unhandled("void", method);
}

static void
blast_CallStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    blast_CallVoidMethodV(env, (jobject)clazz, method, args);
}

static jobject
blast_NewObjectV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    struct dummy_jclass *c = clazz;
    if (c && strcmp(c->name, "android/media/AudioTrack") == 0)
        return (jobject)&audio_track_obj;
    fprintf(stderr, "[BLAST-JNI] UNHANDLED new %s\n", c ? c->name : "?");
    return NULL;
}

/* ── module ────────────────────────────────────────────────────────────────── */

static int
eablast_try_init(struct SupportModule *self)
{
    int found = 0, want = 0;
#define GET(field, sym, type) do {                          \
        self->priv->field = (type)LOOKUP_M(sym);            \
        want++;                                             \
        if (self->priv->field) found++;                     \
    } while (0)

    /* JNI_OnLoad is where an engine RegisterNatives its own callbacks; skipping
     * it fails silently and far from the cause (PORTING-PLAYBOOK.md §2).
     * Prefer the game lib by name in case the apk ships a second .so with a
     * JNI_OnLoad of its own, but fall back to the global lookup so this module
     * is not tied to one EA title. APKENV_BLAST_LIB names it for the next one. */
    {
        const char *lib = getenv("APKENV_BLAST_LIB");
        self->priv->JNI_OnLoad = (jni_onload_t)LOOKUP_LIBM(lib ? lib : "libDeadSpace", "JNI_OnLoad");
        if (self->priv->JNI_OnLoad == NULL)
            self->priv->JNI_OnLoad = (jni_onload_t)LOOKUP_M("JNI_OnLoad");
        want++; if (self->priv->JNI_OnLoad) found++;
    }

    GET(NativeOnCreate,             "_MainActivity_NativeOnCreate",              blast_void_t);
    GET(NativeOnPause,              "_MainActivity_NativeOnPause",               blast_void_t);
    GET(NativeOnResume,             "_MainActivity_NativeOnResume",              blast_void_t);
    GET(NativeOnStop,               "_MainActivity_NativeOnStop",                blast_void_t);
    GET(NativeOnLowMemory,          "_MainActivity_NativeOnLowMemory",           blast_void_t);
    GET(NativeOsExit,               "_MainActivity_NativeOsExit",                blast_void_t);
    GET(NativeGetExitCode,          "_MainActivity_NativeGetExitCode",           blast_int_t);
    GET(NativeOnWindowFocusChanged, "_MainActivity_NativeOnWindowFocusChanged",  blast_bool_arg_t);

    GET(NativeOnSurfaceCreated,     "_AndroidRenderer_NativeOnSurfaceCreated",   blast_void_t);
    GET(NativeOnSurfaceChanged,     "_AndroidRenderer_NativeOnSurfaceChanged",   blast_surfchanged_t);
    GET(NativeOnDrawFrame,          "_AndroidRenderer_NativeOnDrawFrame",        blast_void_t);

    GET(NativeOnPointerEvent,       "_TouchSurfaceAndroid_NativeOnPointerEvent", blast_pointer_t);
    GET(GetIdRawPointerDown,        "_TouchSurfaceAndroid_NativeGetIdRawPointerDown",   blast_int_t);
    GET(GetIdRawPointerUp,          "_TouchSurfaceAndroid_NativeGetIdRawPointerUp",     blast_int_t);
    GET(GetIdRawPointerMove,        "_TouchSurfaceAndroid_NativeGetIdRawPointerMove",   blast_int_t);
    GET(GetIdRawPointerCancel,      "_TouchSurfaceAndroid_NativeGetIdRawPointerCancel", blast_int_t);
    GET(GetIdUndefined,             "_TouchSurfaceAndroid_NativeGetIdUndefined",        blast_int_t);
    GET(GetModuleTypeIdTouchScreen, "_ModuleCatalog_NativeGetModuleTypeIdTouchScreen",  blast_int_t);

    GET(EAThread_Init,              "_EAThread_EAThread_Init",                   blast_void_t);
    GET(EAIO_Startup,               "_EAIO_EAIO_Startup",                        blast_startup_am_t);
    GET(rwfilesystem_Startup,       "_rwfilesystem_rwfilesystem_Startup",        blast_startup_am_t);
    GET(AudioCore_Init,             "_AndroidEAAudioCore_Init",                  blast_audioinit_t);
    GET(NativeOnAcceleration,       "_AccelerometerAndroidDelegate_NativeOnAcceleration", blast_accel_t);
#undef GET

    if (self->priv->NativeOnCreate == NULL || self->priv->NativeOnDrawFrame == NULL ||
        self->priv->NativeOnSurfaceChanged == NULL || self->priv->NativeOnPointerEvent == NULL)
        return 0;

    fprintf(stderr, "[BLAST] try_init: found %d/%d natives (EA BLAST host)\n", found, want);

    /* Pure GLES1 fixed-function; landscape on a landscape device. */
    GLOBAL_M->module_hacks->current_orientation  = ORIENTATION_LANDSCAPE;
    GLOBAL_M->module_hacks->prefer_gles_version  = 1;

    self->override_env.FindClass                = blast_FindClass;
    self->override_env.GetObjectClass           = blast_GetObjectClass;
    self->override_env.ExceptionOccurred        = blast_ExceptionOccurred;
    self->override_env.ExceptionClear           = blast_ExceptionClear;
    self->override_env.ExceptionCheck           = blast_ExceptionCheck;
    self->override_env.NewObjectV               = blast_NewObjectV;
    self->override_env.CallObjectMethodV        = blast_CallObjectMethodV;
    self->override_env.CallStaticObjectMethodV  = blast_CallStaticObjectMethodV;
    self->override_env.CallIntMethodV           = blast_CallIntMethodV;
    self->override_env.CallStaticIntMethodV     = blast_CallStaticIntMethodV;
    self->override_env.CallFloatMethodV         = blast_CallFloatMethodV;
    self->override_env.CallBooleanMethodV       = blast_CallBooleanMethodV;
    self->override_env.CallStaticBooleanMethodV = blast_CallStaticBooleanMethodV;
    self->override_env.CallVoidMethodV          = blast_CallVoidMethodV;
    self->override_env.CallStaticVoidMethodV    = blast_CallStaticVoidMethodV;

    return 1;
}

static void
eablast_init(struct SupportModule *self, int width, int height, const char *home)
{
    struct SupportModulePriv *p = self->priv;
    const char *root;

    global = GLOBAL_M;
    p->screen_w = width;
    p->screen_h = height;
    snprintf(p->home, sizeof(p->home), "%s", home);

    /* The engine opens RELATIVE paths ("published/sounds/soundBase.sb") with
     * plain fopen/mmap — no zip reader, no AAssetManager — so its 319 MB of
     * content has to be real files. apkenv chdir()s to the app dir, so a
     * `published/` shipped there resolves with no copy at all; the delegate
     * answer below covers the case where the engine prefixes a root instead.
     * Override with APKENV_BLAST_CONTENT if it lands somewhere else. */
    root = getenv("APKENV_BLAST_CONTENT");
    if (root == NULL || *root == '\0')
        root = "android/extras";
    /* chdir into the content root rather than copying 319 MB anywhere. The
     * engine's paths are relative ("published/sounds/soundBase.sb"), and it is
     * not certain from the disassembly whether it prefixes them with a
     * delegate's root or uses them bare — so do both: chdir here covers the
     * bare case, and answering GetExternalStorageDirectory with the absolute
     * path below covers the prefixed one.
     *
     * Safe at this point in startup: the bionic libs (APKENV_LOCAL_BIONIC_PATH
     * = "./libs/webos/") and the engine .so are already loaded by the time a
     * module's init() runs, and the packaged log was opened by absolute path. */
    if (chdir(root) == 0 && getcwd(p->content_root, sizeof(p->content_root)) != NULL) {
        fprintf(stderr, "[BLAST] content root: %s (chdir ok)\n", p->content_root);
    } else {
        snprintf(p->content_root, sizeof(p->content_root), "%s", root);
        fprintf(stderr, "[BLAST] WARNING: chdir(%s) failed — the engine will not "
                "find published/ unless it prefixes a delegate root\n", root);
    }
    fprintf(stderr, "[BLAST] home=%s content_root=%s\n", p->home, p->content_root);

    if (p->JNI_OnLoad) {
        fprintf(stderr, "[BLAST] JNI_OnLoad\n");
        p->JNI_OnLoad(VM_M, NULL);
    } else {
        fprintf(stderr, "[BLAST] no JNI_OnLoad — engine callbacks will not be registered\n");
    }

    /* EAIO/rwfilesystem Startup take an AssetManager. The engine imports no
     * AAsset* symbols, so it cannot be doing AAssetManager_fromJava on it —
     * but hand it a real object rather than NULL so nothing can fault on it. */
    if (p->EAThread_Init)        { fprintf(stderr, "[BLAST] EAThread_Init\n");
                                   p->EAThread_Init(ENV_M, GLOBAL_M); }
    if (p->rwfilesystem_Startup) { fprintf(stderr, "[BLAST] rwfilesystem_Startup\n");
                                   p->rwfilesystem_Startup(ENV_M, (jclass)&asset_manager_class,
                                                           (jobject)&asset_manager_class); }
    if (p->EAIO_Startup)         { fprintf(stderr, "[BLAST] EAIO_Startup\n");
                                   p->EAIO_Startup(ENV_M, (jclass)&asset_manager_class,
                                                   (jobject)&asset_manager_class); }

    fprintf(stderr, "[BLAST] NativeOnCreate\n");
    p->NativeOnCreate(ENV_M, GLOBAL_M);

    /* MainActivity.onCreate builds the GLSurfaceView after NativeOnCreate;
     * the renderer then gets surfaceCreated -> surfaceChanged(w,h). */
    if (p->NativeOnSurfaceCreated) {
        fprintf(stderr, "[BLAST] NativeOnSurfaceCreated\n");
        p->NativeOnSurfaceCreated(ENV_M, GLOBAL_M);
    }
    fprintf(stderr, "[BLAST] NativeOnSurfaceChanged(%d, %d)\n", width, height);
    p->NativeOnSurfaceChanged(ENV_M, GLOBAL_M, width, height);

    /* Ask the engine for its own event ids — Java never hardcodes Android's. */
    p->id_down      = p->GetIdRawPointerDown   ? p->GetIdRawPointerDown(ENV_M, GLOBAL_M)   : 0;
    p->id_up        = p->GetIdRawPointerUp     ? p->GetIdRawPointerUp(ENV_M, GLOBAL_M)     : 0;
    p->id_move      = p->GetIdRawPointerMove   ? p->GetIdRawPointerMove(ENV_M, GLOBAL_M)   : 0;
    p->id_cancel    = p->GetIdRawPointerCancel ? p->GetIdRawPointerCancel(ENV_M, GLOBAL_M) : 0;
    p->id_undefined = p->GetIdUndefined        ? p->GetIdUndefined(ENV_M, GLOBAL_M)        : 0;
    p->module_touchscreen = p->GetModuleTypeIdTouchScreen
                          ? p->GetModuleTypeIdTouchScreen(ENV_M, GLOBAL_M) : 0;
    fprintf(stderr, "[BLAST] pointer ids: down=%d up=%d move=%d cancel=%d undefined=%d "
            "touchscreen module=%d\n", p->id_down, p->id_up, p->id_move, p->id_cancel,
            p->id_undefined, p->module_touchscreen);

    /* AndroidEAAudioCore.Startup() equivalent: build the sink, hand it over. */
    blast_audio_open();
    if (p->AudioCore_Init) {
        fprintf(stderr, "[BLAST-AUDIO] AudioCore_Init(track, %d, %d, %d)\n",
                BLAST_AUDIO_RATE, BLAST_AUDIO_CHANNELS, BLAST_AUDIO_BUFBYTES);
        p->AudioCore_Init(ENV_M, (jclass)&audio_track_obj, (jobject)&audio_track_obj,
                          BLAST_AUDIO_RATE, BLAST_AUDIO_CHANNELS, BLAST_AUDIO_BUFBYTES);
    }
}

static void
eablast_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    struct SupportModulePriv *p = self->priv;
    jint raw = p->id_undefined;
    static unsigned long logged = 0;

    if (event == ACTION_DOWN)      raw = p->id_down;
    else if (event == ACTION_UP)   raw = p->id_up;
    else if (event == ACTION_MOVE) raw = p->id_move;

    /* PIXELS — SendRawPointerEvent passes getX(i)/getY(i) undivided. */
    if (logged++ < 40)
        fprintf(stderr, "[BLAST-TOUCH] ev=%d finger=%d (%d,%d) -> raw=%d module=%d\n",
                event, finger, x, y, raw, p->module_touchscreen);

    p->NativeOnPointerEvent(ENV_M, (jclass)GLOBAL_M, raw, p->module_touchscreen,
                            finger, (jfloat)x, (jfloat)y);
}

static void
eablast_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    /* KeyboardAndroid natives exist (NativeOnKeyDown(III)); the TouchPad has no
     * physical keyboard, so left unwired until something asks for it. */
}

static void
eablast_update(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;

    blast_frames++;
    {
        static struct timeval t0;
        static unsigned long mark = 0;
        struct timeval now;
        gettimeofday(&now, NULL);
        if (mark == 0) { t0 = now; mark = blast_frames; }
        else if (blast_frames - mark >= 300) {
            double secs = (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec) / 1e6;
            fprintf(stderr, "[BLAST-FPS] %.1f fps over %lu frames (%.1f ms/frame)\n",
                    (blast_frames - mark) / secs, blast_frames - mark,
                    secs * 1000.0 / (blast_frames - mark));
            t0 = now; mark = blast_frames;
        }
    }

    p->NativeOnDrawFrame(ENV_M, GLOBAL_M);
    if (blast_frames == 1)
        fprintf(stderr, "[BLAST] first NativeOnDrawFrame returned\n");

    if (p->NativeGetExitCode && p->NativeGetExitCode(ENV_M, GLOBAL_M) != 0) {
        fprintf(stderr, "[BLAST] NativeGetExitCode nonzero at frame %lu\n", blast_frames);
        p->want_exit = 1;
    }
}

static void
eablast_pause(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    /* MainActivity's order, read from the caller: onWindowFocusChanged(false)
     * is its own callback, and onPause runs NativeOnPause BEFORE the GL thread
     * stops — the reverse of Mortar. Do not copy the last module here. */
    if (p->NativeOnWindowFocusChanged) p->NativeOnWindowFocusChanged(ENV_M, GLOBAL_M, JNI_FALSE);
    if (p->NativeOnPause)              p->NativeOnPause(ENV_M, GLOBAL_M);
    if (audio_track)                   apkenv_audiotrack_pause(audio_track);
}

static void
eablast_resume(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    if (audio_track)                   apkenv_audiotrack_play(audio_track);
    if (p->NativeOnResume)             p->NativeOnResume(ENV_M, GLOBAL_M);
    if (p->NativeOnWindowFocusChanged) p->NativeOnWindowFocusChanged(ENV_M, GLOBAL_M, JNI_TRUE);
}

static void
eablast_deinit(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
    if (p->NativeOnStop)  p->NativeOnStop(ENV_M, GLOBAL_M);
    if (p->NativeOsExit)  p->NativeOsExit(ENV_M, GLOBAL_M);
    if (audio_track) { apkenv_audiotrack_release(audio_track); audio_track = NULL; }
}

static int
eablast_requests_exit(struct SupportModule *self)
{
    return self->priv->want_exit;
}

APKENV_MODULE(eablast, MODULE_PRIORITY_ENGINE)
