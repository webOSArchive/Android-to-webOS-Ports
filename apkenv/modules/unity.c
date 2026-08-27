
/**
 * apkenv
 * Copyright (c) 2012, Thomas Perl <m@thp.io>
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

/**
 * unity support module 0.1 - by crow_riot
 **/

#include "common.h"
#include "../audio/fmod_pump.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/* engine->host call-out tracer (PORTING-PLAYBOOK.md section 3) */
#define UN_TRACE_MAX 96
static struct { const char *name; unsigned long n; } un_trace[UN_TRACE_MAX];
static int un_trace_n = 0;
static void
un_trace_unhandled(const char *kind, jmethodID method)
{
    int i;
    for (i = 0; i < un_trace_n; i++)
        if (strcmp(un_trace[i].name, method->name) == 0) {
            unsigned long n = ++un_trace[i].n;
            if (n == 100 || n == 10000 || n == 1000000)
                fprintf(stderr, "[UN-JNI] %s %s called %lu times\n", kind, method->name, n);
            return;
        }
    fprintf(stderr, "[UN-JNI] UNHANDLED %s %s%s\n", kind, method->name, method->sig ? method->sig : "");
    if (un_trace_n < UN_TRACE_MAX) { un_trace[un_trace_n].name = strdup(method->name); un_trace[un_trace_n].n = 1; un_trace_n++; }
}
#include <malloc.h>
#include <dlfcn.h>
static void un_hookcheck(const char *where)
{
    fprintf(stderr, "[UN-CHK] %s: __malloc_hook=%p __free_hook=%p __realloc_hook=%p\n", where,
            (void*)__malloc_hook, (void*)__free_hook, (void*)__realloc_hook);
}
/* __malloc_hook watchdog: glibc's malloc hook gets overwritten with ASCII during
 * Mono init. Poll it from a thread and, on change, sample the main thread's
 * user pc (/proc/self/task/<tid>/stat field 30) to localize the writer. */
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
static pid_t un_main_tid;
static void *un_hook_watch(void *arg)
{
    void *last = (void*)__malloc_hook; unsigned long n = 0;
    for (;;) {
        void *cur = (void*)__malloc_hook;
        if (cur != last) {
            char path[64], buf[1024]; snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)un_main_tid);
            FILE *f = fopen(path, "r"); unsigned long pc = 0;
            if (f) { if (fgets(buf, sizeof(buf), f)) { char *q = strrchr(buf, ')'); int i; char *tok = q ? q + 2 : buf;
                       for (i = 3; i < 30 && tok; i++) tok = strchr(tok, ' ') ? strchr(tok, ' ') + 1 : NULL;  /* field 30 = kstkeip */
                       if (tok) pc = strtoul(tok, NULL, 10); } fclose(f); }
            Dl_info di; memset(&di, 0, sizeof(di)); int ok = pc ? apkenv_android_dladdr((void*)pc, &di) : 0;
            fprintf(stderr, "[UN-WATCH] __malloc_hook %p -> %p (poll #%lu); main pc=%p%s%s +0x%x %s\n", last, cur, n, (void*)pc,
                    ok ? " in " : "", ok ? di.dli_fname : "", ok ? (unsigned)(pc - (unsigned long)di.dli_fbase) : 0,
                    (ok && di.dli_sname) ? di.dli_sname : "");
            last = cur;
        }
        n++; usleep(200);
    }
    return NULL;
}
static const char *un_home = "";
static const char *un_pkg = "com.unity3d.player";


static struct GlobalState* global;

/**
 *   jni implementations
 **/

typedef struct
{
    jclass clazz;
    jfieldID field;

} dummy_jobject;


/* -------- */


jfieldID unity_jnienv_GetStaticFieldID(JNIEnv *p0, jclass p1, const char *p2, const char *p3) SOFTFP;
jobject unity_jnienv_GetStaticObjectField(JNIEnv *p0, jclass p1, jfieldID p2) SOFTFP;
jclass unity_jnienv_GetObjectClass(JNIEnv *p0, jobject p1) SOFTFP;
jobject unity_jnienv_CallObjectMethod(JNIEnv* env, jobject p1, jmethodID p2, ...) SOFTFP;
jobject unity_jnienv_CallStaticObjectMethod(JNIEnv* env, jclass p1, jmethodID p2, ...) SOFTFP;
const char * unity_jnienv_GetStringUTFChars(JNIEnv *env, jstring string, jboolean *isCopy) SOFTFP;


jfieldID unity_jnienv_GetStaticFieldID(JNIEnv *p0, jclass p1, const char *p2, const char *p3)
{
    struct dummy_jclass* cls = (struct dummy_jclass*)p1;
    MODULE_DEBUG_PRINTF("GetStaticFieldID %s %s %s\n", cls->name, p2, p3);

    struct _jfieldID* field = malloc(sizeof(struct _jfieldID));

    field->clazz = p1;
    field->name = strdup(p2);
    field->sig = strdup(p3);

    return (jfieldID)field;
}

jobject unity_jnienv_GetStaticObjectField(JNIEnv *p0, jclass p1, jfieldID p2)
{
    struct dummy_jclass* cls = p1;
    struct _jfieldID* fld = (void *)p2;

    MODULE_DEBUG_PRINTF("GetStaticObjectField %s, %s, %s\n", cls->name, fld->name, fld->sig);

    dummy_jobject* obj = malloc(sizeof(dummy_jobject));
    obj->clazz = cls;
    obj->field = p2;

    return obj;
}

jclass unity_jnienv_GetObjectClass(JNIEnv *p0, jobject p1)
{
    MODULE_DEBUG_PRINTF("GetObjectClass %x\n",p1);
    if (p1!=NULL)
    {
        dummy_jobject* obj = p1;
        return obj->clazz;
    }
    return NULL;
}


jobject unity_jnienv_CallObjectMethod(JNIEnv* env, jobject p1, jmethodID p2, ...)
{
    MODULE_DEBUG_PRINTF("CallObjectMethod %x %x\n",p1,p2);

    //dummy_jobject* obj = p1;
    jmethodID method = p2;

    if (strcmp(method->name,"getPackageCodePath")==0)
        return (*env)->NewStringUTF(env, global->apk_filename);

    if (strcmp(method->name,"getFilesDir")==0 || strcmp(method->name,"getCacheDir")==0)
        return (*env)->NewStringUTF(env, un_home);
    if (strcmp(method->name,"getPackageName")==0)
        return (*env)->NewStringUTF(env, un_pkg);
    if (strcmp(method->name,"getCPUType")==0)
        return (*env)->NewStringUTF(env, "ARMv7 VFPv3 NEON");
    if (strcmp(method->name,"getDeviceUniqueIdentifier")==0)
        return (*env)->NewStringUTF(env, "webos-touchpad");
    un_trace_unhandled("obj", method);
    return NULL;
}

jobject
unity_jnienv_CallStaticObjectMethod(JNIEnv* env, jclass p1, jmethodID p2, ...)
{
    struct dummy_jclass* clazz = p1;
    jmethodID method = p2;

    MODULE_DEBUG_PRINTF("unity_jnienv_CallStaticObjectMethod(%s,%s)\n",clazz->name,method->name);

    if (strcmp(method->name,"getProperty")==0) {
        //jstring property = va_arg(p3,jstring);
        //const char* prop = (*env)->
        //dummy_jobject* obj = malloc(sizeof(dummy_jobject));
        //return obj;
        return (*env)->NewStringUTF(env, global->apk_filename);
    }
    else
    if (strcmp(method->name,"getPackageCodePath")==0) {
        return (*env)->NewStringUTF(env, global->apk_filename);
    }

    un_trace_unhandled("staticobj", method);
    return NULL;
}

static void
unity_jnienv_CallVoidMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    un_trace_unhandled("void", p2);
}
static jint
unity_jnienv_CallIntMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (strcmp(p2->name,"getDeviceOrientation")==0) return 0;
    un_trace_unhandled("int", p2);
    return 0;
}
static jboolean
unity_jnienv_CallBooleanMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    un_trace_unhandled("bool", p2);
    return 0;
}
static jfloat
unity_jnienv_CallFloatMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (strcmp(p2->name,"getScreenDPI")==0) return 132.0f;
    un_trace_unhandled("float", p2);
    return 0;
}
jobject
unity_jnienv_NewGlobalRef(JNIEnv* p0, jobject p1)
{
    MODULE_DEBUG_PRINTF("unity_jnienv_NewGlobalRef(%x)\n", p1);
    if (p1==NULL) {
        struct dummy_jclass* cls = malloc(sizeof(struct dummy_jclass));
        cls->name = "null";

        dummy_jobject* obj = malloc(sizeof(dummy_jobject));
        obj->clazz = cls;
        obj->field = NULL;
        MODULE_DEBUG_PRINTF("unity_jnienv_NewGlobalRef(%x) -> %x\n", p1, obj);
        return obj;
    }
    //dummy_jobject
    return NULL;
}




/**
    native calls
 */

typedef void (*unity_nativeInit_t)(JNIEnv* env, jobject p0, jint p1, jint p2);
typedef void (*unity_nativeFile_t)(JNIEnv* env, jobject p0, jstring p1);
typedef jboolean  (*unity_nativeRender_t)(JNIEnv* env, jobject p0);
typedef void (*unity_initJni_t)(JNIEnv* env, jobject p0);
typedef void (*unity_InitPlayerPrefs_t)(JNIEnv*, jobject p0);
typedef jboolean (*unity_androidinit_t)(JNIEnv*, jobject p0, jstring p1, jstring p2);
typedef void (*unity_androidpreparegameloop_t)(JNIEnv*, jobject);
typedef void (*unity_nativeTouch_t)(JNIEnv*, jobject, jint id, jfloat x, jfloat y, jint action, jlong time, jint extra) SOFTFP;
typedef void (*unity_nativeFocusChanged_t)(JNIEnv*, jobject, jboolean) SOFTFP;


/* -------- */

struct SupportModulePriv {
    jni_onload_t JNI_OnLoad_libunity;
    jni_onload_t JNI_OnLoad_libmono;
    unity_initJni_t initJni;
    unity_nativeInit_t nativeInit;
    unity_nativeFile_t nativeFile;
    unity_nativeRender_t nativeRender;
    unity_InitPlayerPrefs_t InitPlayerPrefs;
    unity_androidinit_t unityAndroidInit;
    unity_androidpreparegameloop_t unityAndroidPrepareGameLoop;
    unity_nativeTouch_t nativeTouch;
    unity_nativeFocusChanged_t nativeFocusChanged;
    unsigned long frames;
};
static struct SupportModulePriv unity_priv;

static int
unity_try_init(struct SupportModule *self)
{
    self->priv->JNI_OnLoad_libunity = (jni_onload_t)LOOKUP_LIBM("libunity","JNI_OnLoad");
    /* Always NULL: the apk's libmono.so exports no JNI_OnLoad (checked with nm),
     * and nothing here calls the stored pointer. Kept only for symmetry with
     * libunity above. With APKENV_HOST_MONO set, libmono is not loaded at all. */
    self->priv->JNI_OnLoad_libmono = (jni_onload_t)LOOKUP_LIBM("libmono","JNI_OnLoad");

    self->override_env.GetStaticFieldID = unity_jnienv_GetStaticFieldID;
    self->override_env.GetStaticObjectField = unity_jnienv_GetStaticObjectField;
    self->override_env.GetObjectClass = unity_jnienv_GetObjectClass;
    self->override_env.CallObjectMethod = unity_jnienv_CallObjectMethod;
    self->override_env.CallStaticObjectMethod = unity_jnienv_CallStaticObjectMethod;
    self->override_env.GetStringUTFChars = unity_jnienv_GetStringUTFChars;
    self->override_env.NewGlobalRef = unity_jnienv_NewGlobalRef;
    self->override_env.CallVoidMethodV = unity_jnienv_CallVoidMethodV;
    self->override_env.CallIntMethodV = unity_jnienv_CallIntMethodV;
    self->override_env.CallBooleanMethodV = unity_jnienv_CallBooleanMethodV;
    self->override_env.CallFloatMethodV = unity_jnienv_CallFloatMethodV;

    return (self->priv->JNI_OnLoad_libunity!=NULL);
}


const char *
unity_jnienv_GetStringUTFChars(JNIEnv *env, jstring string, jboolean *isCopy)
{
    MODULE_DEBUG_PRINTF("unity_jnienv_GetStringUTFChars(%x)\n", string);
    if (string == GLOBAL_J(env)) {
        MODULE_DEBUG_PRINTF("WARNING: GetStringUTFChars on global\n");
        return NULL;
    }
    if (string == NULL) {
        return strdup("");
    }
    struct dummy_jstring *str = (struct dummy_jstring*)string;
    MODULE_DEBUG_PRINTF(" \\-> %s\n", str->data);
    return str->data;
}


#define UNITYPLAYER_CLASS_NAME "com/unity3d/player/UnityPlayer"
#define PLAYERPREFS_CLASS_NAME "com/unity3d/player/PlayerPrefs"

static void
unity_init(struct SupportModule *self, int width, int height, const char *home)
{
    global = GLOBAL_M;
    global->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;

    //<unit? or who? does something weird on the pandora ...
    if (setenv("MALLOC_CHECK_", "0", 1) != 0) {
        fprintf(stderr, "Could not set malloc check variable.\n");
        exit(1);
    }

    MODULE_DEBUG_PRINTF("JNI_OnLoad\n");
    un_home = strdup(home);
    un_hookcheck("JNI_OnLoadlibunity)"); fprintf(stderr, "[UN] JNI_OnLoad(libunity)\n");
    self->priv->JNI_OnLoad_libunity(VM_M,0);
    un_hookcheck("JNI_OnLoad done"); fprintf(stderr, "[UN] JNI_OnLoad done\n");
    MODULE_DEBUG_PRINTF("JNI_OnLoad done.\n");

    self->priv->nativeInit = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeInit");
    self->priv->nativeFile = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeFile");
    self->priv->nativeRender = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeRender");
    self->priv->initJni = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "initJni");
    self->priv->unityAndroidInit = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "unityAndroidInit");
    self->priv->unityAndroidPrepareGameLoop = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "unityAndroidPrepareGameLoop");
    self->priv->InitPlayerPrefs = jnienv_find_native_method(PLAYERPREFS_CLASS_NAME, "InitPlayerPrefs");
    self->priv->nativeTouch = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeTouch");
    self->priv->nativeFocusChanged = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeFocusChanged");
    fprintf(stderr, "[UN] natives: init=%p file=%p render=%p initJni=%p androidInit=%p prepare=%p touch=%p prefs=%p\n",
            (void*)self->priv->nativeInit, (void*)self->priv->nativeFile, (void*)self->priv->nativeRender,
            (void*)self->priv->initJni, (void*)self->priv->unityAndroidInit, (void*)self->priv->unityAndroidPrepareGameLoop,
            (void*)self->priv->nativeTouch, (void*)self->priv->InitPlayerPrefs);

    /* Java UnityPlayer order: <init>: nativeFile(apk); a(): initJni, PlayerPrefs, nativeInitWWW;
     * GL thread: nativeInit(w,h); first onDrawFrame: unityAndroidInit("assets/bin/", dataDir/lib),
     * unityAndroidPrepareGameLoop; then nativeRender() per frame. */
    jstring file = GLOBAL_M->env->NewStringUTF(ENV_M,global->apk_filename);
    un_hookcheck("nativeFile"); fprintf(stderr, "[UN] nativeFile(%s)\n", global->apk_filename);
    self->priv->nativeFile(ENV_M,GLOBAL_M,file);
    if (self->priv->initJni) { un_hookcheck("initJni"); fprintf(stderr, "[UN] initJni\n"); self->priv->initJni(ENV_M,GLOBAL_M); }
    if (self->priv->InitPlayerPrefs) { un_hookcheck("InitPlayerPrefs"); fprintf(stderr, "[UN] InitPlayerPrefs\n"); self->priv->InitPlayerPrefs(ENV_M,GLOBAL_M); }
    un_hookcheck("nativeInit"); fprintf(stderr, "[UN] nativeInit(%d,%d)\n", width, height);
    self->priv->nativeInit(ENV_M,GLOBAL_M,width,height);
    char libdir[512]; snprintf(libdir, sizeof(libdir), "%slib", home);
    jstring bin = GLOBAL_M->env->NewStringUTF(ENV_M,"assets/bin/");
    jstring lib = GLOBAL_M->env->NewStringUTF(ENV_M,libdir);
    un_main_tid = (pid_t)syscall(SYS_gettid);
    { pthread_t wt; pthread_create(&wt, NULL, un_hook_watch, NULL); }
    un_hookcheck("unityAndroidInitassets"); fprintf(stderr, "[UN] unityAndroidInit(assets/bin/, %s)\n", libdir);
    self->priv->unityAndroidInit(ENV_M,GLOBAL_M,bin,lib);
    fprintf(stderr, "[UN] unityAndroidInit done\n");
    if (self->priv->unityAndroidPrepareGameLoop) {
        fprintf(stderr, "[UN] unityAndroidPrepareGameLoop\n");
        self->priv->unityAndroidPrepareGameLoop(ENV_M,GLOBAL_M);
        fprintf(stderr, "[UN] prepare done\n");
    }
    if (self->priv->nativeFocusChanged) self->priv->nativeFocusChanged(ENV_M,GLOBAL_M,1);
    /* FMOD audio: same AudioTrack-style device pump as WMW (org.fmod.FMODAudioDevice) */
    if (!(getenv("APKENV_UNITY_AUDIO") && getenv("APKENV_UNITY_AUDIO")[0]=='0'))
        apkenv_fmod_pump_start(GLOBAL_M);
}
static jlong
un_now_ms(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (jlong)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
unity_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    /* UnityPlayer.dispatchTouchEvent -> nativeTouch(pointerId, x, y, action&0xff, eventTime, extra)
     * Android actions: 0=DOWN 1=UP 2=MOVE */
    int action = (event == ACTION_DOWN) ? 0 : (event == ACTION_UP) ? 1 : 2;
    if (self->priv->nativeTouch)
        self->priv->nativeTouch(ENV_M, GLOBAL_M, finger, (jfloat)x, (jfloat)y, action, un_now_ms(), 0);
}

static void
unity_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
}

static void
unity_update(struct SupportModule *self)
{
    if (self->priv->nativeRender) {
        jboolean r = self->priv->nativeRender(ENV_M,GLOBAL_M);
        self->priv->frames++;
        if (self->priv->frames <= 3 || (self->priv->frames % 600) == 0)
            fprintf(stderr, "[UN] nativeRender #%lu -> %d\n", self->priv->frames, r);
    }
}

static void
unity_deinit(struct SupportModule *self)
{
}

static void
unity_pause(struct SupportModule *self)
{
}

static void
unity_resume(struct SupportModule *self)
{
}

static int
unity_requests_exit(struct SupportModule *self)
{
    return 0;
}

APKENV_MODULE(unity, MODULE_PRIORITY_ENGINE)




