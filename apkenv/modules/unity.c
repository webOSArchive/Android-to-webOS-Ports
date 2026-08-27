
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
#include <time.h>
#include <limits.h>
extern void apkenv_gl_probe_frame(unsigned long frame);

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
/* Cached from init() so the JNI handlers can answer getDisplaySize(). */
static int un_screen_w = 0, un_screen_h = 0;
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
jobject unity_jnienv_CallObjectMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3) SOFTFP;
jobject unity_jnienv_CallStaticObjectMethod(JNIEnv* env, jclass p1, jmethodID p2, ...) SOFTFP;
jobject unity_jnienv_CallStaticObjectMethodV(JNIEnv* env, jclass p1, jmethodID p2, va_list p3) SOFTFP;
static jobject unity_call_static_object(JNIEnv *env, jclass p1, jmethodID p2) SOFTFP;
static jobject unity_call_object(JNIEnv *env, jmethodID method, va_list *ap) SOFTFP;
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


/* ---------------------------------------------------------------------------
 * PlayerPrefs (com.unity3d.player.UnityPlayer's Java preference store).
 *
 * Unity's managed PlayerPrefs.SetX() THROWS PlayerPrefsException when the Java
 * side returns false, and the game calls it from AudioManager.Awake(),
 * GameController.Awake() and Promotion.BeginPromo() - so leaving these
 * unimplemented (returning 0) aborts the game's own startup objects. This is a
 * real store, persisted next to the rest of the app's data, so settings and
 * progress survive a restart.
 * ------------------------------------------------------------------------- */
enum un_pref_type { UN_PREF_INT, UN_PREF_FLOAT, UN_PREF_STRING };

struct un_pref {
    char *key;
    enum un_pref_type type;
    int i;
    float f;
    char *s;
};

#define UN_PREFS_MAX 512
static struct un_pref un_prefs[UN_PREFS_MAX];
static int un_prefs_n = 0;
static char un_prefs_path[PATH_MAX];
static int un_prefs_dirty = 0;

static struct un_pref *
un_pref_find(const char *key)
{
    int i;
    if (key == NULL)
        return NULL;
    for (i = 0; i < un_prefs_n; i++)
        if (strcmp(un_prefs[i].key, key) == 0)
            return &un_prefs[i];
    return NULL;
}

static struct un_pref *
un_pref_slot(const char *key)
{
    struct un_pref *p = un_pref_find(key);
    if (p != NULL) {
        if (p->type == UN_PREF_STRING) { free(p->s); p->s = NULL; }
        return p;
    }
    if (un_prefs_n >= UN_PREFS_MAX) {
        fprintf(stderr, "[UN-PREF] table full (%d), dropping '%s'\n", UN_PREFS_MAX, key);
        return NULL;
    }
    p = &un_prefs[un_prefs_n++];
    p->key = strdup(key);
    p->s = NULL;
    return p;
}

/* Line format: <type>\t<key>\t<value>. Keys cannot contain a tab (Unity keys
 * are identifiers), and string values may not contain a newline - they are
 * escaped on write. */
static void
un_prefs_save(void)
{
    FILE *f;
    int i;

    if (!un_prefs_dirty || un_prefs_path[0] == '\0')
        return;

    f = fopen(un_prefs_path, "w");
    if (f == NULL) {
        fprintf(stderr, "[UN-PREF] cannot write %s\n", un_prefs_path);
        return;
    }
    for (i = 0; i < un_prefs_n; i++) {
        struct un_pref *p = &un_prefs[i];
        switch (p->type) {
        case UN_PREF_INT:    fprintf(f, "i\t%s\t%d\n", p->key, p->i); break;
        case UN_PREF_FLOAT:  fprintf(f, "f\t%s\t%.9g\n", p->key, p->f); break;
        case UN_PREF_STRING: {
            const char *c;
            fprintf(f, "s\t%s\t", p->key);
            for (c = p->s ? p->s : ""; *c; c++) {
                if (*c == '\n')      fputs("\\n", f);
                else if (*c == '\\') fputs("\\\\", f);
                else                 fputc(*c, f);
            }
            fputc('\n', f);
            break;
        }
        }
    }
    fclose(f);
    un_prefs_dirty = 0;
}

static void
un_prefs_load(const char *home)
{
    FILE *f;
    char line[4096];

    snprintf(un_prefs_path, sizeof(un_prefs_path), "%s/playerprefs.txt", home);
    f = fopen(un_prefs_path, "r");
    if (f == NULL) {
        fprintf(stderr, "[UN-PREF] no store yet at %s (fresh profile)\n", un_prefs_path);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *key, *val, *nl;
        struct un_pref *p;
        if ((nl = strchr(line, '\n')) != NULL) *nl = '\0';
        if (line[0] == '\0' || line[1] != '\t') continue;
        key = line + 2;
        val = strchr(key, '\t');
        if (val == NULL) continue;
        *val++ = '\0';
        p = un_pref_slot(key);
        if (p == NULL) break;
        switch (line[0]) {
        case 'i': p->type = UN_PREF_INT;   p->i = atoi(val); break;
        case 'f': p->type = UN_PREF_FLOAT; p->f = (float)atof(val); break;
        case 's': {
            char *r = val, *w = val;
            while (*r) {
                if (*r == '\\' && r[1] == 'n')      { *w++ = '\n'; r += 2; }
                else if (*r == '\\' && r[1] == '\\') { *w++ = '\\'; r += 2; }
                else                                 { *w++ = *r++; }
            }
            *w = '\0';
            p->type = UN_PREF_STRING; p->s = strdup(val);
            break;
        }
        default: break;
        }
    }
    fclose(f);
    fprintf(stderr, "[UN-PREF] loaded %d preferences from %s\n", un_prefs_n, un_prefs_path);
}

/* Unwrap a jstring argument already pulled from a va_list. NULL for the global
 * sentinel (jni/jnienv.c hands it out for unanswered object calls) so callers
 * can tell "no value" from "empty string". */
static const char *
un_jstr(JNIEnv *env, void *arg)
{
    struct dummy_jstring *js = arg;
    if (js == NULL || (void *)js == (void *)GLOBAL_J(env))
        return NULL;
    return js->data;
}

/* Shared dispatch for the object-returning instance calls.
 *
 * BOTH the "..." and the va_list ("V") entry points must route here. Overriding
 * only CallObjectMethod leaves CallObjectMethodV on the generic fallback in
 * jni/jnienv.c, which returns GLOBAL_J(env) - the GlobalState pointer used as a
 * sentinel - for *every* unanswered call. libunity then hands that to
 * GetStringUTFChars, gets NULL back, and constructs a std::string from it:
 * SIGSEGV in strlen with no clue as to which host method was really wanted.
 * Returning NULL here instead is both honest and traceable. */
static jobject
unity_call_object(JNIEnv *env, jmethodID method, va_list *ap)
{
    /* PlayerPrefs.GetString(key, default) */
    if (ap != NULL && strcmp(method->name,"GetString")==0) {
        const char *key = un_jstr(env, va_arg(*ap, void *));
        const char *def = un_jstr(env, va_arg(*ap, void *));
        struct un_pref *p = un_pref_find(key);
        if (p != NULL && p->type == UN_PREF_STRING)
            return (*env)->NewStringUTF(env, p->s ? p->s : "");
        return (*env)->NewStringUTF(env, def ? def : "");
    }

    /* int[]{width,height}. Returning NULL made Unity log
     * "JNI: Init'd AndroidJavaObject with null ptr!" and fall back to guesses. */
    if (strcmp(method->name,"getDisplaySize")==0) {
        jintArray a = (*env)->NewIntArray(env, 2);
        struct dummy_array *da = (struct dummy_array *)a;
        if (da != NULL && da->data != NULL) {
            ((jint *)da->data)[0] = un_screen_w;
            ((jint *)da->data)[1] = un_screen_h;
        }
        return a;
    }
    /* No gamepads on a TouchPad: an EMPTY array, not NULL - Unity iterates it. */
    if (strcmp(method->name,"getConnectedJoysticks")==0)
        return (*env)->NewIntArray(env, 0);

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

jobject unity_jnienv_CallObjectMethod(JNIEnv* env, jobject p1, jmethodID p2, ...)
{
    jobject r;
    va_list ap;
    MODULE_DEBUG_PRINTF("CallObjectMethod %x %x\n",p1,p2);
    va_start(ap, p2);
    r = unity_call_object(env, p2, &ap);
    va_end(ap);
    return r;
}

jobject unity_jnienv_CallObjectMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("CallObjectMethodV %s/%s\n", p2->name, p2->sig ? p2->sig : "");
    return unity_call_object(env, p2, &p3);
}

jobject
unity_call_static_object(JNIEnv *env, jclass p1, jmethodID p2)
{
    struct dummy_jclass* clazz = p1;
    jmethodID method = p2;

    MODULE_DEBUG_PRINTF("unity_call_static_object(%s,%s)\n",clazz->name,method->name);

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

jobject
unity_jnienv_CallStaticObjectMethod(JNIEnv* env, jclass p1, jmethodID p2, ...)
{
    return unity_call_static_object(env, p1, p2);
}

jobject
unity_jnienv_CallStaticObjectMethodV(JNIEnv* env, jclass p1, jmethodID p2, va_list p3)
{
    return unity_call_static_object(env, p1, p2);
}

static void
unity_jnienv_CallVoidMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    /* PlayerPrefs.Save()/Flush() - the game's explicit "persist now". */
    if (strcmp(p2->name,"Flush")==0 || strcmp(p2->name,"Save")==0 ||
        strcmp(p2->name,"Sync")==0) {
        un_prefs_save();
        return;
    }
    if (strcmp(p2->name,"DeleteKey")==0) {
        const char *key = un_jstr(env, va_arg(p3, void *));
        struct un_pref *pr = un_pref_find(key);
        if (pr != NULL) {
            int idx = (int)(pr - un_prefs);
            free(pr->key);
            if (pr->type == UN_PREF_STRING) free(pr->s);
            un_prefs[idx] = un_prefs[--un_prefs_n];
            un_prefs_dirty = 1;
        }
        return;
    }
    if (strcmp(p2->name,"DeleteAll")==0) {
        int i;
        for (i = 0; i < un_prefs_n; i++) {
            free(un_prefs[i].key);
            if (un_prefs[i].type == UN_PREF_STRING) free(un_prefs[i].s);
        }
        un_prefs_n = 0;
        un_prefs_dirty = 1;
        un_prefs_save();
        return;
    }
    /* UnityPlayer.setOrientation() only forwards to Android's
     * Activity.setRequestedOrientation() - a windowing request with no return
     * value the engine consumes, so a no-op is correct here (apkenv owns the
     * framebuffer). The VALUE is worth knowing though: it is what the game
     * thinks its orientation should be. ActivityInfo constants:
     *   0=LANDSCAPE 1=PORTRAIT 6=SENSOR_LANDSCAPE 7=SENSOR_PORTRAIT
     *   8=REVERSE_LANDSCAPE 9=REVERSE_PORTRAIT */
    if (strcmp(p2->name,"setOrientation")==0) {
        int o = va_arg(p3, int);
        static int last = -1;
        if (o != last) {
            last = o;
            fprintf(stderr, "[UN] setOrientation(%d) = %s (no-op; apkenv owns the framebuffer)\n",
                    o, (o == 1 || o == 7 || o == 9) ? "PORTRAIT" :
                       (o == 0 || o == 6 || o == 8) ? "LANDSCAPE" : "other");
        }
        return;
    }
    /* Host UI affordances with no webOS equivalent; silently fine as no-ops.
     * Named here so they stop showing up as unanswered contract gaps. */
    if (strcmp(p2->name,"startActivityIndicator")==0 ||
        strcmp(p2->name,"stopActivityIndicator")==0 ||
        strcmp(p2->name,"setWakeLock")==0)
        return;

    un_trace_unhandled("void", p2);
}
static jint
unity_jnienv_CallIntMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (strcmp(p2->name,"getDeviceOrientation")==0) return 0;
    /* 0 = ORIENTATION_UNDEFINED; the TouchPad is landscape-native and apkenv
     * already owns rotation, so never report a change here. */
    if (strcmp(p2->name,"getOrientation")==0) return 0;
    /* Bytes of RAM Unity may assume it can use. The device has ~940 MB total;
     * report a conservative 256 MB so Unity picks modest texture/heap budgets. */
    if (strcmp(p2->name,"getTotalMemory")==0) return 256 * 1024 * 1024;

    if (strcmp(p2->name,"GetInt")==0) {          /* PlayerPrefs.GetInt(key, def) */
        const char *key = un_jstr(env, va_arg(p3, void *));
        int def = va_arg(p3, int);
        struct un_pref *p = un_pref_find(key);
        return (p != NULL && p->type == UN_PREF_INT) ? p->i : def;
    }

    un_trace_unhandled("int", p2);
    return 0;
}
static jboolean
unity_jnienv_CallBooleanMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    /* Every PlayerPrefs setter must return TRUE; managed PlayerPrefs.SetX()
     * throws PlayerPrefsException on false, which kills the caller's Awake(). */
    if (strcmp(p2->name,"SetInt")==0) {
        const char *key = un_jstr(env, va_arg(p3, void *));
        int v = va_arg(p3, int);
        struct un_pref *p = un_pref_slot(key);
        if (p == NULL) return 0;
        p->type = UN_PREF_INT; p->i = v; un_prefs_dirty = 1;
        return 1;
    }
    if (strcmp(p2->name,"SetFloat")==0) {
        const char *key = un_jstr(env, va_arg(p3, void *));
        float v = (float)va_arg(p3, double);
        struct un_pref *p = un_pref_slot(key);
        if (p == NULL) return 0;
        p->type = UN_PREF_FLOAT; p->f = v; un_prefs_dirty = 1;
        return 1;
    }
    if (strcmp(p2->name,"SetString")==0) {
        const char *key = un_jstr(env, va_arg(p3, void *));
        const char *v = un_jstr(env, va_arg(p3, void *));
        struct un_pref *p = un_pref_slot(key);
        if (p == NULL) return 0;
        p->type = UN_PREF_STRING; p->s = strdup(v ? v : ""); un_prefs_dirty = 1;
        return 1;
    }
    if (strcmp(p2->name,"HasKey")==0) {
        const char *key = un_jstr(env, va_arg(p3, void *));
        return un_pref_find(key) != NULL;
    }

    un_trace_unhandled("bool", p2);
    return 0;
}
static jfloat
unity_jnienv_CallFloatMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (strcmp(p2->name,"getScreenDPI")==0) return 132.0f;

    if (strcmp(p2->name,"GetFloat")==0) {        /* PlayerPrefs.GetFloat(key, def) */
        const char *key = un_jstr(env, va_arg(p3, void *));
        /* jfloat through C varargs is promoted to double - read it as such. */
        float def = (float)va_arg(p3, double);
        struct un_pref *p = un_pref_find(key);
        return (p != NULL && p->type == UN_PREF_FLOAT) ? p->f : def;
    }

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
/* UnityPlayer.onSurfaceChanged() -> nativeResize(surfaceW, surfaceH, viewW, viewH)
 * (signature (IIII)V, read out of libunity's JNINativeMethod table and confirmed
 * against UnityPlayer.smali). */
typedef void (*unity_nativeResize_t)(JNIEnv*, jobject, jint, jint, jint, jint);
typedef void (*unity_nativeResume_t)(JNIEnv*, jobject);            /* ()V  */
typedef void (*unity_nativeFocusChanged_t)(JNIEnv*, jobject, jboolean); /* (Z)V */
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
    unity_nativeResize_t nativeResize;
    unity_nativeResume_t nativeResume;
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

    /* Unity 3.5 links both GLES libs but draws through shaders only. */
    GLOBAL_M->module_hacks->prefer_gles_version = 2;

    self->override_env.GetStaticFieldID = unity_jnienv_GetStaticFieldID;
    self->override_env.GetStaticObjectField = unity_jnienv_GetStaticObjectField;
    self->override_env.GetObjectClass = unity_jnienv_GetObjectClass;
    self->override_env.CallObjectMethod = unity_jnienv_CallObjectMethod;
    self->override_env.CallObjectMethodV = unity_jnienv_CallObjectMethodV;
    self->override_env.CallStaticObjectMethod = unity_jnienv_CallStaticObjectMethod;
    self->override_env.CallStaticObjectMethodV = unity_jnienv_CallStaticObjectMethodV;
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
    /* Returning NULL here is a live crash source: libunity feeds the result
     * straight into a std::string ctor, which strlen()s it. Log every NULL
     * return loudly so it shows up in the trail instead of as a SIGSEGV in
     * strlen with no context. */
    if (string == GLOBAL_J(env)) {
        fprintf(stderr, "[UN-JNI] GetStringUTFChars on GLOBAL ref -> NULL\n");
        return NULL;
    }
    if (string == NULL) {
        fprintf(stderr, "[UN-JNI] GetStringUTFChars(NULL) -> \"\"\n");
        if (isCopy) *isCopy = JNI_TRUE;
        return strdup("");
    }
    struct dummy_jstring *str = (struct dummy_jstring*)string;
    if (str->data == NULL)
        fprintf(stderr, "[UN-JNI] GetStringUTFChars: jstring %p has NULL data\n",
                (void *)string);
    MODULE_DEBUG_PRINTF(" \\-> %s\n", str->data);
    /* Return a COPY, per the JNI contract - jni/jnienv.c does the same and says
     * why. ReleaseStringUTFChars() free()s whatever we hand back, so returning
     * str->data directly frees the jstring's OWN buffer: the next Get on that
     * jstring reads freed memory, and the allocator aborts with
     * "double free or corruption (fasttop)". That is exactly what happened on
     * the first touch here - Unity Get/Releases the same jstring more than once. */
    if (isCopy) *isCopy = JNI_TRUE;
    return strdup(str->data ? str->data : "");
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
    un_prefs_load(un_home);
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
    self->priv->nativeResize = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeResize");
    self->priv->nativeResume = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeResume");
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
    un_screen_w = width; un_screen_h = height;
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
    /* UnityPlayer.onDrawFrame()'s first-time tail, in the Java host's exact
     * order (read from UnityPlayer.smali):
     *     unityAndroidInit -> unityAndroidPrepareGameLoop
     *     -> nativeResize(w, h, w, h) -> nativeResume() -> windowFocusChanged(true)
     * Skipping the middle two leaves the engine sized 0x0 AND paused: it clears
     * to the camera background every frame and submits no geometry at all
     * (measured: 0 glDrawArrays, glViewport never called). nativeResize must come
     * AFTER prepareGameLoop - calling it next to nativeInit is too early, the
     * graphics device does not exist yet and the size is dropped. */
    if (self->priv->nativeResize) {
        fprintf(stderr, "[UN] nativeResize(%d,%d,%d,%d)\n", width, height, width, height);
        self->priv->nativeResize(ENV_M,GLOBAL_M,width,height,width,height);
    } else {
        fprintf(stderr, "[UN] WARNING: nativeResize not found - expect a blank screen\n");
    }
    if (self->priv->nativeResume) {
        fprintf(stderr, "[UN] nativeResume\n");
        self->priv->nativeResume(ENV_M,GLOBAL_M);
    } else {
        fprintf(stderr, "[UN] WARNING: nativeResume not found - engine stays paused\n");
    }
    if (self->priv->nativeFocusChanged) self->priv->nativeFocusChanged(ENV_M,GLOBAL_M,1);
    /* FMOD audio: same AudioTrack-style device pump as WMW (org.fmod.FMODAudioDevice) */
    if (!(getenv("APKENV_UNITY_AUDIO") && getenv("APKENV_UNITY_AUDIO")[0]=='0'))
        apkenv_fmod_pump_start(GLOBAL_M);
}
/* Android's MotionEvent.getEventTime() is SystemClock.uptimeMillis(), a
 * monotonic clock. Use the same base here so every time we hand the engine
 * comes from ONE clock (Stage T, plan/TEMPLERUN2-RENDER-INPUT.md). */
static jlong
un_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
unity_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    /* UnityPlayer.dispatchTouchEvent(pointerIndex, rawAction, pointerId, x, y, eventTime, source)
     *   -> nativeTouch(pointerId, x, y, action&0xff, eventTime, source)
     * Android actions: 0=DOWN 1=UP 2=MOVE. The trailing int is the MotionEvent
     * SOURCE: UnityPlayer.onTouchEvent hard-codes 0x1002 (InputDevice.SOURCE_TOUCHSCREEN)
     * when no touchpad handler is installed. Passing 0 (what we did until 0.1.2)
     * is a source the engine never sees on a real device. */
    enum { ANDROID_SOURCE_TOUCHSCREEN = 0x1002 };
    int action = (event == ACTION_DOWN) ? 0 : (event == ACTION_UP) ? 1 : 2;
    jlong t = un_now_ms();
    static int logged = 0;
    if (logged < 200 || action != 2) {   /* every DOWN/UP; first 200 MOVEs */
        fprintf(stderr, "[UN-TOUCH] id=%d action=%d x=%d y=%d t=%lld src=0x%x %s\n",
                finger, action, x, y, (long long)t, ANDROID_SOURCE_TOUCHSCREEN,
                self->priv->nativeTouch ? "" : "(nativeTouch NULL - dropped)");
        logged++;
    }
    if (self->priv->nativeTouch)
        self->priv->nativeTouch(ENV_M, GLOBAL_M, finger, (jfloat)x, (jfloat)y, action, t,
                                ANDROID_SOURCE_TOUCHSCREEN);
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
        if (self->priv->frames <= 3 || (self->priv->frames % 600) == 0) {
            fprintf(stderr, "[UN] nativeRender #%lu -> %d\n", self->priv->frames, r);
            apkenv_gl_probe_frame(self->priv->frames);
        }
        /* The app is killed rather than closed on webOS (and during dev by the
         * harness), so PlayerPrefs would never reach disk if we only saved on
         * Flush(). Write back periodically; un_prefs_save() is a no-op unless
         * something actually changed. */
        if ((self->priv->frames % 300) == 0)
            un_prefs_save();
    }
}

static void
unity_deinit(struct SupportModule *self)
{
    un_prefs_save();
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




