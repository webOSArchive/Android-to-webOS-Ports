
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
#include "../apklib/apklib.h"
#include "../audio/fmod_pump.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <GLES2/gl2.h>
#include "../accelerometer/accelerometer.h"

int apkenv_fbo_es2_rotation(void);   /* compat/fbo_es2.c */
GLuint apkenv_fbo_es2_ensure(void);
#include "../compat/hooks.h"
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
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
/* A call through a jmethodID that GetMethodID never produced (NULL): the
 * engine looked a host method up, got nothing, and called it anyway. It used to
 * fault on method->name (robocop-r5: SIGSEGV addr=0x4 in CallVoidMethodV). The
 * caller's address names the engine site. */
static void
un_null_method(const char *kind, void *ra)
{
    static int n;
    if (n++ < 20)
        fprintf(stderr, "[UN-JNI] %s call with a NULL jmethodID from %p - ignored\n", kind, ra);
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
/* What getPackageName() answers. The real host returns the game's own package;
 * APKENV_UNITY_PACKAGE supplies it (Temple Run 2 shipped with the default). */
static const char *un_pkg = "com.unity3d.player";
/* Unity 4 and Unity 3.5 hosts differ in their first-frame order (see
 * un_first_frame_tail). Detected from the engine's own native table, not from a
 * version string: nativeSetInputCanceled(Z) exists only in the Unity 4 host. */
static int un_unity4 = 0;
/* Unity 4.2 host: jvalue[] AndroidJavaObject calls (see the 4.2 section). */
static int un_unity42 = 0;
/* APKENV_UNITY_OBB_CODEPATH: the OBB stands in for the apk (see unity_init). */
static int un_obb_codepath = 0;
static const char *un_codepath = NULL;


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
static jobject unity_call_static_object(JNIEnv *env, jclass p1, jmethodID p2, va_list *ap) SOFTFP;
static jobject unity_call_object(JNIEnv *env, jobject obj, jmethodID method, va_list *ap) SOFTFP;
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

/* The music bed in audio/fmod_pump.c (APKENV_FMOD_MUSIC_PCM) is mixed in
 * *underneath* the engine, so nothing in the engine scales it - including the
 * game's own music volume setting. It does not have to stay that way: a Unity
 * game keeps that setting in PlayerPrefs, and PlayerPrefs is ours. Name the
 * float key in APKENV_FMOD_MUSIC_PREF and every SetFloat on it drives the bed's
 * gain live, so the in-game slider works and 0 means off.
 *
 * Temple Run 2 writes "TR Music Volume" (mirrored into its own gamedata.txt as
 * "MusicVolume"). A game that keeps the setting only in its own save file, or
 * inside Unity's managed PlayerPrefs rather than the Java store, would need a
 * different publisher - this hook is the cheap case, not the only one. */
static void
un_pref_publish_music(const char *key, float value)
{
    static const char *want = NULL;
    static int looked_up = 0;

    if (!looked_up) {
        want = getenv("APKENV_FMOD_MUSIC_PREF");
        looked_up = 1;
    }
    if (want == NULL || want[0] == '\0' || key == NULL)
        return;
    if (strcmp(key, want) == 0)
        apkenv_fmod_music_set_volume(value);
}

/* Startup: the store on disk already holds the setting from the last session,
 * and the game may never call SetFloat again if the player does not touch it. */
static void
un_pref_publish_all_music(void)
{
    int i;
    for (i = 0; i < un_prefs_n; i++)
        if (un_prefs[i].type == UN_PREF_FLOAT)
            un_pref_publish_music(un_prefs[i].key, un_prefs[i].f);
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
    un_pref_publish_all_music();
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

/* ---- Unity 4: AndroidJavaObject bridge ------------------------------------
 * Managed AndroidJavaObject/AndroidJavaClass resolve every member through the
 * Java host's com.unity3d.player.ReflectionHelper (getMethodID / getFieldID /
 * getConstructorID -> java.lang.reflect.*), then JNI FromReflectedMethod/Field,
 * and call through the jvalue[] ("A") entry points. With none of that answered,
 * every AndroidJavaObject call logs "Unable to find method id" and yields 0 or
 * NULL - harmless for Temple Run 2's analytics plugins, fatal for Aralon's UI:
 * its DisplayMetricsAndroid reads the screen size this way and GuiMgr sizes
 * the entire GUI from it (0x0 -> no menu drawn, no touch ever hits).
 *
 * We answer with small made-up objects, not a Java VM: a reflected member
 * carries (class, name, sig) straight into the jmethodID/jfieldID that the
 * rest of the fake JNI already dispatches on by name, and an object carries its
 * class so GetObjectClass and getClass().getName() - which Unity uses to build
 * call signatures - tell the truth. Gated on the Unity 4 host so the shipped
 * Temple Run 2 path keeps its exact behaviour until it gets a regression run. */
#define UN_REFL_MAGIC 0x55524631u
#define UN_OBJ_MAGIC  0x554f424au
/* The Java object is the Thread: AndroidWWW joins it before freeing the
 * request, so join() must really wait - returning early let the engine free
 * state our worker was still calling back into. Begins like un_obj. */
#define UN_WWW_MAGIC 0x55575757u
struct un_www_obj {
    jclass clazz;
    jfieldID field;
    unsigned magic;
    pthread_t thread;
    int joined;
};

/* Both begin like dummy_jobject {clazz, field}, so unity_jnienv_GetObjectClass
 * works on them unchanged. */
struct un_refl {
    jclass clazz;
    jfieldID field;
    unsigned magic;
    struct _jmethodID id;       /* what FromReflectedMethod/Field hand out */
};
struct un_obj {
    jclass clazz;
    jfieldID field;
    unsigned magic;
    const char *described;      /* java.lang.Class objects: the class described */
};

static int
un_is(const void *p, unsigned magic)
{
    return p != NULL && ((const struct un_obj *)p)->magic == magic;
}

static const char *
un_clsname(jclass c)
{
    return c != NULL ? ((struct dummy_jclass *)c)->name : "?";
}

static jclass
un_class(const char *name)
{
    struct dummy_jclass *c = malloc(sizeof(*c));
    c->name = strdup(name);
    return c;
}

static jobject
un_new_obj(const char *cls, const char *described)
{
    struct un_obj *o = calloc(1, sizeof(*o));
    o->clazz = un_class(cls);
    o->magic = UN_OBJ_MAGIC;
    o->described = described ? strdup(described) : NULL;
    return o;
}

/* One line per call, bounded: this is the Unity 4 contract being discovered as
 * the game exercises it. */
static void
un_refl_log(const char *what, const char *cls, const char *name, const char *extra)
{
    static int n;
    if (n < 120) {
        n++;
        fprintf(stderr, "[UN-REFL] %s %s.%s %s\n", what, cls ? cls : "?",
                name ? name : "?", extra ? extra : "");
    }
}

/* ReflectionHelper.getMethodID(Class, String name, String sig, boolean static)
 *                 .getFieldID (Class, String name, String sig, boolean static)
 *                 .getConstructorID(Class, String sig) */
/* Argument cursor over either calling form: the va_list ("V") entry points or
 * the jvalue[] ("A") ones. Unity 4.0 (Aralon) reached ReflectionHelper through
 * the V forms; Unity 4.2 (RoboCop) makes EVERY AndroidJavaObject call through the
 * A forms, which jni/jnienv.c answers with NULL/0 - so Class.forName came back
 * NULL and the Glu plugin layer threw in Boot.Awake (plan/logs/robocop-r2.log). */
struct un_args {
    va_list *ap;
    const jvalue *jv;
    int i;
};

static void *
un_arg_ptr(struct un_args *a)
{
    if (a->jv != NULL) return a->jv[a->i++].l;
    return a->ap != NULL ? va_arg(*a->ap, void *) : NULL;
}

static int un_java_call(JNIEnv *env, const char *cls, jobject obj, jmethodID m,
                        struct un_args *a, jvalue *out);
static void un_java_unanswered(const char *kind, const char *cls, jmethodID m);
static jobject un_standin(const char *kind, const char *cls, jmethodID m);
static int un_endswith(const char *s, const char *suffix);

/* java.lang.Class objects made by Class.forName stand in for the jclass of
 * AndroidJavaClass, so a "class" may be either kind: name the one described. */
static const char *
un_described(jclass c)
{
    if (un_is(c, UN_OBJ_MAGIC) && ((struct un_obj *)c)->described != NULL)
        return ((struct un_obj *)c)->described;
    return un_clsname(c);
}

static jobject
un_reflect(JNIEnv *env, jmethodID method, struct un_args *a)
{
    int ctor = strcmp(method->name, "getConstructorID") == 0;
    jclass cls = un_arg_ptr(a);
    const char *name = ctor ? "<init>" : un_jstr(env, un_arg_ptr(a));
    const char *sig = un_jstr(env, un_arg_ptr(a));
    struct un_refl *r = calloc(1, sizeof(*r));

    r->clazz = un_class(ctor ? "java/lang/reflect/Constructor" :
                        strcmp(method->name, "getFieldID") == 0 ? "java/lang/reflect/Field" :
                                                                 "java/lang/reflect/Method");
    r->magic = UN_REFL_MAGIC;
    r->id.clazz = cls;
    r->id.name = strdup(name ? name : "?");
    r->id.sig = strdup(sig ? sig : "");
    un_refl_log(method->name, un_described(cls), r->id.name, r->id.sig);
    return r;
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
unity_call_object(JNIEnv *env, jobject obj, jmethodID method, va_list *ap)
{
    if (method == NULL) { un_null_method("obj", __builtin_return_address(0)); return NULL; }
    /* Unity 4 AndroidJavaObject chains (see the bridge above):
     *   currentActivity.getWindowManager().getDefaultDisplay().getMetrics(dm)
     * and getClass().getName(), which Unity calls on every AndroidJavaObject
     * argument to build the JNI signature it then looks up. */
    if (un_unity42 && ap != NULL) {
        va_list copy;
        struct un_args a = { &copy, NULL, 0 };
        jvalue v;
        int ok;
        va_copy(copy, *ap);
        ok = un_java_call(env, obj != NULL ? un_described(((dummy_jobject *)obj)->clazz) : "?",
                          obj, method, &a, &v);
        va_end(copy);
        if (ok) return v.l;
    }
    if (un_unity4) {
        if (strcmp(method->name,"getWindowManager")==0) {
            un_refl_log("call", "Activity", method->name, "-> WindowManager");
            return un_new_obj("android/view/WindowManager", NULL);
        }
        if (strcmp(method->name,"getDefaultDisplay")==0) {
            un_refl_log("call", "WindowManager", method->name, "-> Display");
            return un_new_obj("android/view/Display", NULL);
        }
        if (strcmp(method->name,"getClass")==0) {
            const char *c = obj != NULL ? un_clsname(((dummy_jobject *)obj)->clazz) : "java/lang/Object";
            return un_new_obj("java/lang/Class", c);
        }
        if (strcmp(method->name,"getName")==0 && un_is(obj, UN_OBJ_MAGIC) &&
            ((struct un_obj *)obj)->described != NULL) {
            char dotted[256], *p;
            snprintf(dotted, sizeof(dotted), "%s", ((struct un_obj *)obj)->described);
            for (p = dotted; *p; p++) if (*p == '/') *p = '.';
            un_refl_log("call", "Class", "getName", dotted);
            return (*env)->NewStringUTF(env, dotted);
        }
    }

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
        return (*env)->NewStringUTF(env, un_codepath ? un_codepath : global->apk_filename);

    /* Unity 4.2: Android's <data>/files and <data>/cache, real directories
     * (persistentDataPath / temporaryCachePath; the content pipeline commits
     * its A/B resolution and caches bundles there). Older hosts: unchanged. */
    if (un_unity42 && (strcmp(method->name,"getFilesDir")==0 || strcmp(method->name,"getCacheDir")==0)) {
        char path[PATH_MAX];
        size_t l;
        snprintf(path, sizeof(path), "%s", un_home);
        l = strlen(path);
        while (l > 1 && path[l - 1] == '/') path[--l] = '\0';
        snprintf(path + l, sizeof(path) - l, "/%s",
                 strcmp(method->name,"getFilesDir")==0 ? "files" : "cache");
        mkdir(path, 0755);
        return (*env)->NewStringUTF(env, path);
    }
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
    r = unity_call_object(env, p1, p2, &ap);
    va_end(ap);
    return r;
}

jobject unity_jnienv_CallObjectMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("CallObjectMethodV %s/%s\n", p2->name, p2->sig ? p2->sig : "");
    return unity_call_object(env, p1, p2, &p3);
}

jobject
unity_call_static_object(JNIEnv *env, jclass p1, jmethodID p2, va_list *ap)
{
    if (p2 == NULL) { un_null_method("staticobj", __builtin_return_address(0)); return NULL; }
    struct dummy_jclass* clazz = p1;
    jmethodID method = p2;

    MODULE_DEBUG_PRINTF("unity_call_static_object(%s,%s)\n",clazz->name,method->name);

    if (un_unity42 && ap != NULL) {
        va_list copy;
        struct un_args a = { &copy, NULL, 0 };
        jvalue v;
        int ok;
        va_copy(copy, *ap);
        ok = un_java_call(env, un_described(p1), NULL, method, &a, &v);
        va_end(copy);
        if (ok) return v.l;
    }
    /* com.unity3d.player.ReflectionHelper - see the AndroidJavaObject bridge. */
    if (un_unity4 && ap != NULL &&
        (strcmp(method->name,"getMethodID")==0 || strcmp(method->name,"getFieldID")==0 ||
         strcmp(method->name,"getConstructorID")==0)) {
        struct un_args a = { ap, NULL, 0 };
        return un_reflect(env, method, &a);
    }

    if (strcmp(method->name,"getProperty")==0) {
        //jstring property = va_arg(p3,jstring);
        //const char* prop = (*env)->
        //dummy_jobject* obj = malloc(sizeof(dummy_jobject));
        //return obj;
        return (*env)->NewStringUTF(env, global->apk_filename);
    }
    else
    if (strcmp(method->name,"getPackageCodePath")==0) {
        return (*env)->NewStringUTF(env, un_codepath ? un_codepath : global->apk_filename);
    }

    un_trace_unhandled("staticobj", method);
    return NULL;
}

jobject
unity_jnienv_CallStaticObjectMethod(JNIEnv* env, jclass p1, jmethodID p2, ...)
{
    jobject r;
    va_list ap;
    va_start(ap, p2);
    r = unity_call_static_object(env, p1, p2, &ap);
    va_end(ap);
    return r;
}

jobject
unity_jnienv_CallStaticObjectMethodV(JNIEnv* env, jclass p1, jmethodID p2, va_list p3)
{
    return unity_call_static_object(env, p1, p2, &p3);
}

static void
unity_jnienv_CallVoidMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (p2 == NULL) { un_null_method("void", __builtin_return_address(0)); return ; }
    if (un_unity42) {
        va_list copy;
        struct un_args a = { &copy, NULL, 0 };
        jvalue v;
        int ok;
        va_copy(copy, p3);
        ok = un_java_call(env, p1 != NULL ? un_described(((dummy_jobject *)p1)->clazz) : "?", p1, p2, &a, &v);
        va_end(copy);
        if (ok) return;
    }
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
    /* Display.getMetrics(DisplayMetrics): the fields are answered when read
     * (unity_jnienv_GetIntField/GetFloatField), so there is nothing to fill. */
    if (un_unity4 && strcmp(p2->name,"getMetrics")==0) {
        un_refl_log("call", "Display", "getMetrics", "(fields answered on read)");
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
        strcmp(p2->name,"setWakeLock")==0 ||
        /* Unity 4 additions: gyro drift compensation, and the soft-keyboard
         * text push (only reachable once a soft keyboard is shown). */
        strcmp(p2->name,"enableSensorCompensation")==0 ||
        strcmp(p2->name,"setSoftInputStr")==0)
        return;

    un_trace_unhandled("void", p2);
}
/* ---- portrait ---------------------------------------------------------------
 * Temple Run 2's manifest declares portrait on every Unity activity, and its
 * UI is authored for it: on the landscape panel the settings screen overlaps
 * itself. The engine is told it has a 768x1024 portrait surface and renders
 * into an offscreen FBO of that size; compat/fbo_es2.c rotates it onto the
 * 1024x768 panel at present time. 768x1024 rotated is exactly 1024x768, so it
 * fills the panel with no letterboxing and no scaling.
 *
 * APKENV_UNITY_PORTRAIT=0 goes back to native landscape. */
static int
un_portrait(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("APKENV_UNITY_PORTRAIT");
        on = (e == NULL || e[0] != '0');
    }
    return on;
}

static jint
unity_jnienv_CallIntMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (p2 == NULL) { un_null_method("int", __builtin_return_address(0)); return 0; }
    if (strcmp(p2->name,"getDeviceOrientation")==0) return 0;
    /* Surface.ROTATION_0: the surface we hand the engine is already the right
     * way up for the orientation we report, because we rotate at present time. */
    /* 0 = ORIENTATION_UNDEFINED; the TouchPad is landscape-native and apkenv
     * already owns rotation, so never report a change here. */
    if (strcmp(p2->name,"getOrientation")==0)
        return un_portrait() ? 1 : 0;   /* Configuration.ORIENTATION_PORTRAIT */
    /* Bytes of RAM Unity may assume it can use. The device has ~940 MB total;
     * report a conservative 256 MB so Unity picks modest texture/heap budgets. */
    if (strcmp(p2->name,"getTotalMemory")==0) return 256 * 1024 * 1024;
    /* Unity 4: Display.getRotation() in degrees. We hand the engine a surface
     * that is already the right way up (landscape native, or rotated at present
     * time for portrait), so it is 0 either way. No cameras on this path. */
    if (strcmp(p2->name,"getScreenOrientationAngle")==0) return 0;
    if (strcmp(p2->name,"getCameraOrientation")==0) return 0;
    /* Unity 4.2: Display handed over by nativeSetDefaultDisplay. */
    if (un_unity42 && strcmp(p2->name,"getWidth")==0) return un_screen_w;
    if (un_unity42 && strcmp(p2->name,"getHeight")==0) return un_screen_h;

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
    if (p2 == NULL) { un_null_method("bool", __builtin_return_address(0)); return 0; }
    if (un_unity42) {
        va_list copy;
        struct un_args a = { &copy, NULL, 0 };
        jvalue v;
        int ok;
        va_copy(copy, p3);
        ok = un_java_call(env, p1 != NULL ? un_described(((dummy_jobject *)p1)->clazz) : "?", p1, p2, &a, &v);
        va_end(copy);
        if (ok) return v.z;
    }
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
        un_pref_publish_music(key, v);
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
    /* Unity 4 capability queries: no vibrator is driven, no gyro compensation. */
    if (strcmp(p2->name,"vibrationSupported")==0 ||
        strcmp(p2->name,"isSensorCompensationEnabled")==0)
        return 0;

    un_trace_unhandled("bool", p2);
    return 0;
}
static jfloat
unity_jnienv_CallFloatMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    if (p2 == NULL) { un_null_method("float", __builtin_return_address(0)); return 0; }
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
    /* A global ref to one of our objects IS the object: nothing moves or is
     * collected. Returning NULL is what made every Unity 4 AndroidJavaObject log
     * "Init'd AndroidJavaObject with null ptr!". 3.5 behaviour unchanged. */
    return un_unity4 ? p1 : NULL;
}

/* ---- Unity 4 AndroidJavaObject bridge: the JNI entry points ---------------
 * Each falls back to exactly what jni/jnienv.c answered before when the host
 * is not Unity 4, so Temple Run 2 sees no change. */
static jmethodID
unity_jnienv_FromReflectedMethod(JNIEnv *env, jobject m)
{
    if (un_unity4 && un_is(m, UN_REFL_MAGIC))
        return &((struct un_refl *)m)->id;
    if (un_unity4) un_refl_log("FromReflectedMethod", "?", "(foreign object)", "-> NULL");
    return NULL;
}

static jfieldID
unity_jnienv_FromReflectedField(JNIEnv *env, jobject f)
{
    if (un_unity4 && un_is(f, UN_REFL_MAGIC))
        return (jfieldID)&((struct un_refl *)f)->id;
    if (un_unity4) un_refl_log("FromReflectedField", "?", "(foreign object)", "-> NULL");
    return NULL;
}

/* Constructors arrive through getConstructorID -> FromReflectedMethod. */
static jobject
un_construct(jclass cls, jmethodID m)
{
    /* un_described: on Unity 4.2 the class is a Class.forName object, and reading
     * it as a dummy_jclass named the new object after heap garbage (robocop-r4). */
    const char *c = cls != NULL ? un_described(cls) : (m != NULL ? un_described(m->clazz) : "?");
    un_refl_log("new", c, "<init>", (m != NULL && m->sig != NULL) ? m->sig : "");
    return un_new_obj(c, NULL);
}

static jobject un_www_new(JNIEnv *env, jclass cls, struct un_args *a);

static jobject
unity_jnienv_NewObjectA(JNIEnv *env, jclass cls, jmethodID m, jvalue *args)
{
    if (un_unity42 && un_endswith(un_described(cls), "unity3d/player/WWW")) {
        struct un_args a = { NULL, args, 0 };
        return un_www_new(env, cls, &a);
    }
    return un_unity4 ? un_construct(cls, m) : NULL;           /* jnienv.c: NULL */
}

static jobject
unity_jnienv_NewObjectV(JNIEnv *env, jclass cls, jmethodID m, va_list args)
{
    if (un_unity42 && un_endswith(un_described(cls), "unity3d/player/WWW")) {
        struct un_args a = { &args, NULL, 0 };
        return un_www_new(env, cls, &a);
    }
    return un_unity4 ? un_construct(cls, m) : GLOBAL_J(env);  /* jnienv.c: sentinel */
}

/* jvalue[] forms dispatch on the method name exactly like the va_list forms.
 * The only handlers that read arguments (PlayerPrefs) never arrive this way. */
static jobject
unity_jnienv_CallObjectMethodA(JNIEnv *env, jobject obj, jmethodID m, jvalue *args)
{
    if (!un_unity4)
        return NULL;                                           /* jnienv.c: NULL */
    if (un_unity42) {
        struct un_args a = { NULL, args, 0 };
        jvalue v;
        const char *c = obj != NULL ? un_described(((dummy_jobject *)obj)->clazz) : "?";
        jobject r;
        if (un_java_call(env, c, obj, m, &a, &v))
            return v.l;
        r = unity_call_object(env, obj, m, NULL);
        return r != NULL ? r : un_standin("Object", c, m);
    }
    return unity_call_object(env, obj, m, NULL);
}

static void
unity_jnienv_CallVoidMethodA(JNIEnv *env, jobject obj, jmethodID m, jvalue *args)
{
    if (!un_unity4)
        return;
    if (strcmp(m->name, "getMetrics") == 0) {
        un_refl_log("call", "Display", "getMetrics", "(fields answered on read)");
        return;
    }
    if (un_unity42) {
        struct un_args a = { NULL, args, 0 };
        jvalue v;
        const char *c = obj != NULL ? un_described(((dummy_jobject *)obj)->clazz) : "?";
        if (!un_java_call(env, c, obj, m, &a, &v))
            un_java_unanswered("Void", c, m);
        return;
    }
    un_trace_unhandled("voidA", m);
}

/* android.util.DisplayMetrics for this device: the surface the engine was
 * given (un_screen_w/h), the panel's physical 132 dpi, and the mdpi density
 * bucket (1.0 / 160) that Android assigns a 132-dpi screen. GuiMgr:configure
 * lays out from width/height; GuiMgr:useLargeScreen divides widthPixels by
 * xdpi (1024 / 132 = 7.8 inches). */
static jint
unity_jnienv_GetIntField(JNIEnv *env, jobject obj, jfieldID f)
{
    const char *n = f != NULL ? ((struct _jmethodID *)f)->name : NULL;
    jint v;
    char s[32];

    if (!un_unity4 || n == NULL)
        return 0;
    if      (strcmp(n, "widthPixels") == 0)  v = un_screen_w;
    else if (strcmp(n, "heightPixels") == 0) v = un_screen_h;
    else if (strcmp(n, "densityDpi") == 0)   v = 160;
    else {
        un_refl_log("GetIntField", "?", n, "-> 0 (unanswered)");
        return 0;
    }
    snprintf(s, sizeof(s), "-> %d", (int)v);
    un_refl_log("GetIntField", "DisplayMetrics", n, s);
    return v;
}

static jfloat
unity_jnienv_GetFloatField(JNIEnv *env, jobject obj, jfieldID f)
{
    const char *n = f != NULL ? ((struct _jmethodID *)f)->name : NULL;
    float v;
    char s[32];

    if (!un_unity4 || n == NULL)
        return 0.0f;
    if      (strcmp(n, "density") == 0 || strcmp(n, "scaledDensity") == 0) v = 1.0f;
    else if (strcmp(n, "xdpi") == 0 || strcmp(n, "ydpi") == 0)              v = 132.0f;
    else {
        un_refl_log("GetFloatField", "?", n, "-> 0 (unanswered)");
        return 0.0f;
    }
    snprintf(s, sizeof(s), "-> %.1f", v);
    un_refl_log("GetFloatField", "DisplayMetrics", n, s);
    return v;
}




/**
    native calls
 */

/* ---- Unity 4.2: the jvalue[] ("A") JNI forms + the Glu plugin layer -------
 * Gated on un_unity42 (the host has nativeSetDefaultDisplay, new in 4.2), so
 * Aralon's Unity 4.0 path keeps exactly what it shipped with.
 *
 * Every AndroidJavaObject/AndroidJavaClass call arrives here as (class, method
 * name, jvalue[]). Anything we do not answer is logged once as [UN-JAVA] with
 * its class and signature, so the next gap names itself.
 * Answers and their evidence: plan/ROBOCOP.md (static contract, 2026-09-17). */
static int
un_endswith(const char *s, const char *suffix)
{
    size_t ls = s ? strlen(s) : 0, lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static void
un_java_unanswered(const char *kind, const char *cls, jmethodID m)
{
    #define UN_JAVA_SEEN_MAX 256
    static char *seen[UN_JAVA_SEEN_MAX];
    static int n;
    char key[512];
    int i;
    snprintf(key, sizeof(key), "%s.%s%s", cls ? cls : "?", m->name, m->sig ? m->sig : "");
    for (i = 0; i < n; i++)
        if (strcmp(seen[i], key) == 0)
            return;
    if (n < UN_JAVA_SEEN_MAX) seen[n++] = strdup(key);
    fprintf(stderr, "[UN-JAVA] unanswered %s %s\n", kind, key);
}

/* Glu's AJTUtil.GetRunCount: launches so far, counted by the host (Glu's Java
 * kept it in SharedPreferences). 0 forever would keep the first-launch flows. */
static int un_glu_runcount = 0;
#define UN_GLU_RUNCOUNT_KEY "apkenv.glu.runcount"

static const char *
un_env_or(const char *name, const char *def)
{
    const char *e = getenv(name);
    return (e != NULL && e[0] != '\0') ? e : def;
}

/* Returns 1 and fills *out if answered. `cls` is the Java class the call is on
 * (static: the class; instance: the object's class), slash-separated. */
static int
un_java_call(JNIEnv *env, const char *cls, jobject obj, jmethodID m,
             struct un_args *a, jvalue *out)
{
    const char *n = m->name;
    out->j = 0;

    /* AndroidJavaClass(name) = Class.forName(name): a Class object that
     * describes the class, used as the jclass of every later static call. */
    if (strcmp(n, "forName") == 0 && un_endswith(cls, "java/lang/Class")) {
        const char *name = un_jstr(env, un_arg_ptr(a));
        char slashed[256], *p;
        snprintf(slashed, sizeof(slashed), "%s", name ? name : "?");
        for (p = slashed; *p; p++) if (*p == '.') *p = '/';
        un_refl_log("call", "Class", "forName", slashed);
        out->l = un_new_obj("java/lang/Class", slashed);
        return 1;
    }

    /* WWW threads (see the host WWW section). */
    if (un_is(obj, UN_WWW_MAGIC) && strcmp(n, "join") == 0) {
        struct un_www_obj *w = (struct un_www_obj *)obj;
        if (!w->joined) { pthread_join(w->thread, NULL); w->joined = 1; }
        return 1;
    }
    if (un_is(obj, UN_WWW_MAGIC) && strcmp(n, "isAlive") == 0) {
        struct un_www_obj *w = (struct un_www_obj *)obj;
        out->z = !w->joined && pthread_kill(w->thread, 0) == 0;
        return 1;
    }
    if (strcmp(n, "loadLibrary") == 0) { out->z = 1; return 1; }  /* P/Invoke: see apkenv.c fallback */

    /* --- com.glu.plugins.* (AJavaTools) --- */
    /* getFilesDir().getPath() / getExternalFilesDir(null).getPath(): one data
     * dir serves both on webOS (it lives on the media partition already). */
    if (strcmp(n, "GetFilesPath") == 0 || strcmp(n, "GetExternalFilesPath") == 0) {
        /* Android's layout, <data>/files beside <data>/shared_prefs: Glu's
         * GWalletHelper writes "<files>/../shared_prefs/GWalletHelper.xml"
         * (robocop-r5: DirectoryNotFoundException). */
        char path[PATH_MAX];
        size_t l;
        snprintf(path, sizeof(path), "%s", un_home);
        l = strlen(path);
        while (l > 1 && path[l - 1] == '/') path[--l] = '\0';
        snprintf(path + l, sizeof(path) - l, "/shared_prefs");
        mkdir(path, 0755);
        snprintf(path + l, sizeof(path) - l, "/files");
        mkdir(path, 0755);
        out->l = (*env)->NewStringUTF(env, path);
        return 1;
    }
    if (strcmp(n, "GetDeviceLanguage") == 0) { out->l = (*env)->NewStringUTF(env, "en"); return 1; }
    if (strcmp(n, "GetDeviceCountry") == 0 || strcmp(n, "getCurrentCountryISO") == 0) {
        out->l = (*env)->NewStringUTF(env, "US");
        return 1;
    }
    if (strcmp(n, "GetPackageName") == 0) { out->l = (*env)->NewStringUTF(env, un_pkg); return 1; }
    if (strcmp(n, "GetVersionName") == 0) {
        out->l = (*env)->NewStringUTF(env, un_env_or("APKENV_UNITY_VERSION_NAME", "1.0"));
        return 1;
    }
    if (strcmp(n, "GetVersionCode") == 0) {
        out->i = atoi(un_env_or("APKENV_UNITY_VERSION_CODE", "1"));
        return 1;
    }
    if (strcmp(n, "GetRunCount") == 0) { out->i = un_glu_runcount; return 1; }
    /* AJTUtil.GetOBBDownloadPlan: "old" = the installed OBB is current (the
     * Java default once the version code is recorded). null threw in
     * Util.LogEventOBB (eventName) inside App.Awake. */
    if (strcmp(n, "GetOBBDownloadPlan") == 0) { out->l = (*env)->NewStringUTF(env, "old"); return 1; }
    if (strcmp(n, "GetAndroidID") == 0) { out->l = (*env)->NewStringUTF(env, "0f1e2d3c4b5a6978"); return 1; }
    if (strcmp(n, "elapsedRealtime") == 0 || strcmp(n, "uptimeMillis") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        out->j = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        return 1;
    }
    /* Glu build properties (res/raw/properties.dat, AES in the Java host). The
     * C# side falls back to defaults on null; the two that steer behaviour: */
    if (strcmp(n, "GetProperty") == 0 && un_endswith(cls, "AJTUtil")) {
        const char *key = un_jstr(env, un_arg_ptr(a));
        const char *v = NULL;
        if (key != NULL && strcmp(key, "DEVELOPMENT_BUILD") == 0) v = "false";
        else if (key != NULL && strcmp(key, "BUILD_TYPE") == 0) v = "google";
        un_refl_log("call", "AJTUtil", "GetProperty", key);
        out->l = v != NULL ? (*env)->NewStringUTF(env, v) : NULL;
        return 1;
    }
    /* GooglePlayServicesUtil result: 1 = SERVICE_MISSING (what ACL reports).
     * 0 would mean "available" and start a sign-in that waits for a callback. */
    if (strcmp(n, "IsPlayGameServicesAvailable") == 0) { out->i = 1; return 1; }
    if (strcmp(n, "IsDeviceRooted") == 0 || strcmp(n, "IsGluDebug") == 0 ||
        strcmp(n, "isPayerUser") == 0) {
        out->z = 0;
        return 1;
    }
    if (strcmp(n, "GetScreenDiagonalInches") == 0) { out->d = 9.7; return 1; }

    /* --- java.util.Locale: Application.systemLanguage, from engine code.
     * A NULL string here aborts libunity in std::string. --- */
    if (strcmp(n, "getDefault") == 0 && un_endswith(cls, "java/util/Locale")) {
        out->l = un_new_obj("java/util/Locale", NULL);
        return 1;
    }
    if (strcmp(n, "getISO3Language") == 0) { out->l = (*env)->NewStringUTF(env, "eng"); return 1; }
    if (strcmp(n, "getLanguage") == 0 && un_endswith(cls, "java/util/Locale")) {
        out->l = (*env)->NewStringUTF(env, "en");
        return 1;
    }
    if (strcmp(n, "getCountry") == 0 && un_endswith(cls, "java/util/Locale")) {
        out->l = (*env)->NewStringUTF(env, "US");
        return 1;
    }

    /* --- NativeUtils.MT_GetCurrentMemoryBytes, polled by FpsCounter.Update:
     * ActivityManager.getProcessMemoryInfo(new int[]{Process.myPid()})[0]
     * .getTotalPss(). A NULL array threw IndexOutOfRangeException on nearly
     * every frame (124 in a short play session, robocop-r13). --- */
    if (strcmp(n, "myPid") == 0) { out->i = getpid(); return 1; }
    if (strcmp(n, "getProcessMemoryInfo") == 0) {
        jintArray arr = (*env)->NewIntArray(env, 1);      /* pointer-sized slots */
        struct dummy_array *da = (struct dummy_array *)arr;
        if (da != NULL && da->data != NULL)
            ((void **)da->data)[0] = un_new_obj("android/os/Debug$MemoryInfo", NULL);
        out->l = arr;
        return 1;
    }
    if (strcmp(n, "getTotalPss") == 0) {                 /* kB; VmRSS, re-read once a second */
        static int kb;
        static time_t last;
        time_t now = time(NULL);
        if (now != last) {
            char line[128];
            FILE *f = fopen("/proc/self/status", "r");
            last = now;
            while (f != NULL && fgets(line, sizeof(line), f) != NULL)
                if (sscanf(line, "VmRSS: %d kB", &kb) == 1) break;
            if (f != NULL) fclose(f);
        }
        out->i = kb;
        return 1;
    }

    /* --- android.* chains --- */
    if (strcmp(n, "getWidth") == 0 && un_endswith(cls, "Display")) { out->i = un_screen_w; return 1; }
    if (strcmp(n, "getHeight") == 0 && un_endswith(cls, "Display")) { out->i = un_screen_h; return 1; }
    if (strcmp(n, "getPackageManager") == 0) {
        out->l = un_new_obj("android/content/pm/PackageManager", NULL);
        return 1;
    }
    if (strcmp(n, "getPackageInfo") == 0) {
        out->l = un_new_obj("android/content/pm/PackageInfo", NULL);
        return 1;
    }
    return 0;
}

/* An unanswered plugin call that returns an object (a factory, a manager
 * instance): hand back a stand-in of the declared return class rather than
 * NULL. Managed code wraps the result in an AndroidJavaObject, and a NULL there
 * throws "Init'd AndroidJavaObject with null ptr!" and aborts the caller's
 * Awake - Glu's App.Awake set up every plugin after the ads factory this way
 * (robocop-r3). Calls made on the stand-in come back through un_java_call and
 * are logged like any other. Strings and arrays stay NULL: a fake object there
 * would be read as character or element data. */
static jobject
un_standin(const char *kind, const char *cls, jmethodID m)
{
    const char *ret = m->sig != NULL ? strchr(m->sig, ')') : NULL;
    char name[256];
    size_t l;
    un_java_unanswered(kind, cls, m);
    if (ret == NULL || ret[1] != 'L' || strcmp(ret + 1, "Ljava/lang/String;") == 0)
        return NULL;
    snprintf(name, sizeof(name), "%s", ret + 2);
    l = strlen(name);
    if (l > 0 && name[l - 1] == ';') name[l - 1] = '\0';
    return un_new_obj(name, NULL);
}

/* The A entry points. Static calls pass the class; instance calls the object. */
static jvalue
un_call_a(JNIEnv *env, const char *kind, jclass cls, jobject obj, jmethodID m,
          const jvalue *args, int *answered)
{
    struct un_args a = { NULL, args, 0 };
    const char *c = cls != NULL ? un_described(cls) :
                    obj != NULL ? un_described(((dummy_jobject *)obj)->clazz) : "?";
    jvalue v;
    *answered = un_java_call(env, c, obj, m, &a, &v);
    if (!*answered) {
        v.j = 0;
    }
    return v;
}

#define UN_A_STATIC(ret, Name, field, fallback)                                   \
static ret                                                                        \
unity_jnienv_CallStatic##Name##MethodA(JNIEnv *env, jclass cls, jmethodID m, jvalue *args) \
{                                                                                 \
    int ok;                                                                       \
    jvalue v;                                                                     \
    if (!un_unity42) return fallback;                                             \
    v = un_call_a(env, "static", cls, NULL, m, args, &ok);                        \
    if (!ok) un_java_unanswered("static " #Name, un_described(cls), m);           \
    return (ret)v.field;                                                          \
}
#define UN_A_INSTANCE(ret, Name, field, fallback)                                 \
static ret                                                                        \
unity_jnienv_Call##Name##MethodA(JNIEnv *env, jobject obj, jmethodID m, jvalue *args) \
{                                                                                 \
    int ok;                                                                       \
    jvalue v;                                                                     \
    if (!un_unity42) return fallback;                                             \
    v = un_call_a(env, "instance", NULL, obj, m, args, &ok);                      \
    if (!ok) un_java_unanswered(#Name, obj ? un_described(((dummy_jobject *)obj)->clazz) : "null", m); \
    return (ret)v.field;                                                          \
}
UN_A_STATIC(jboolean, Boolean, z, 0)
UN_A_STATIC(jint, Int, i, 0)
UN_A_STATIC(jlong, Long, j, 0)
UN_A_STATIC(jfloat, Float, f, 0.0f)
UN_A_STATIC(jdouble, Double, d, 0.0)
UN_A_INSTANCE(jboolean, Boolean, z, 0)
UN_A_INSTANCE(jint, Int, i, 0)
UN_A_INSTANCE(jlong, Long, j, 0)
UN_A_INSTANCE(jfloat, Float, f, 0.0f)
UN_A_INSTANCE(jdouble, Double, d, 0.0)

static jobject
unity_jnienv_CallStaticObjectMethodA(JNIEnv *env, jclass cls, jmethodID m, jvalue *args)
{
    int ok;
    jvalue v;
    if (!un_unity42)
        return NULL;                                           /* jnienv.c: NULL */
    if (strcmp(m->name, "getMethodID") == 0 || strcmp(m->name, "getFieldID") == 0 ||
        strcmp(m->name, "getConstructorID") == 0) {
        struct un_args a = { NULL, args, 0 };
        return un_reflect(env, m, &a);
    }
    v = un_call_a(env, "static", cls, NULL, m, args, &ok);
    if (ok) return v.l;
    return un_standin("static Object", un_described(cls), m);
}

static void
unity_jnienv_CallStaticVoidMethodA(JNIEnv *env, jclass cls, jmethodID m, jvalue *args)
{
    int ok;
    if (!un_unity42)
        return;
    un_call_a(env, "static", cls, NULL, m, args, &ok);
    if (!ok) un_java_unanswered("static Void", un_described(cls), m);
}

/* Object arrays we build (getProcessMemoryInfo) are pointer-sized int arrays. */
static jobject
unity_jnienv_GetObjectArrayElement(JNIEnv *env, jobjectArray a, jsize i)
{
    struct dummy_array *da = (struct dummy_array *)a;
    if (!un_unity42 || da == NULL || da->data == NULL || i < 0 || i >= da->length ||
        da->element_size != sizeof(void *))
        return NULL;                                           /* jnienv.c: NULL */
    return ((void **)da->data)[i];
}

static jobject
unity_jnienv_GetObjectField(JNIEnv *env, jobject obj, jfieldID f)
{
    const char *n = f != NULL ? ((struct _jmethodID *)f)->name : NULL;
    if (!un_unity42 || n == NULL)
        return NULL;                                           /* jnienv.c: NULL */
    if (strcmp(n, "versionName") == 0)                         /* PackageInfo */
        return (*env)->NewStringUTF(env, un_env_or("APKENV_UNITY_VERSION_NAME", "1.0"));
    un_refl_log("GetObjectField", obj ? un_described(((dummy_jobject *)obj)->clazz) : "?",
                n, "-> NULL (unanswered)");
    return NULL;
}

/* ---- Unity 4.2: host WWW (com.unity3d.player.WWW) ---------------------------
 * On Android, UnityEngine.WWW is Java: the host calls nativeInitWWW(WWW.class),
 * the engine binds its callbacks there, and each request is
 * `new WWW(id, url, postData, headers)` - a Thread that opens the URL and streams
 * it back through the static natives. apkenv never called nativeInitWWW, so every
 * WWW - including LOCAL ones - neither completed nor failed. RoboCop's content
 * pipeline resolves its A/B test offline from
 * WWW("jar:file://<obb>!/assets/ABTesting.xml"); unresolved, it raised
 * "ABTesting resolution is not valid" (the "SLOW INTERNET CONNECTION" screen).
 *
 * This follows WWW.run() from the game's own smali for the protocols a device
 * with no network actually serves: file:// and jar:file://<zip>!/<entry> are
 * read and streamed (content-length header, 32 KB readCallback chunks after a
 * leading zero-length one, progress, done); anything else fails with the
 * exception text Java would produce offline. */
typedef jboolean (*un_www_header_t)(JNIEnv *, jclass, jint, jstring) SOFTFP;
typedef jboolean (*un_www_read_t)(JNIEnv *, jclass, jint, jbyteArray, jint) SOFTFP;
typedef void (*un_www_progress_t)(JNIEnv *, jclass, jint, jfloat, jfloat, jdouble, jint) SOFTFP;
typedef void (*un_www_done_t)(JNIEnv *, jclass, jint) SOFTFP;
typedef void (*un_www_error_t)(JNIEnv *, jclass, jint, jstring) SOFTFP;

#define UN_WWW_CLASS "com/unity3d/player/WWW"
static jclass un_www_class;
static pthread_mutex_t un_www_zip_lock = PTHREAD_MUTEX_INITIALIZER;

struct un_www_req {
    JNIEnv *env;
    int id;
    char *url;
};


/* One open zip per archive path, reused: apklib has no close, and unzip
 * handles are not thread-safe, hence the lock around every read. */
static int
un_www_read_zip(const char *zip, const char *entry, char **buf, size_t *len)
{
    #define UN_WWW_ZIPS 4
    static struct { char *path; AndroidApk *apk; } zips[UN_WWW_ZIPS];
    AndroidApk *apk = NULL;
    int i, ok;

    pthread_mutex_lock(&un_www_zip_lock);
    for (i = 0; i < UN_WWW_ZIPS && zips[i].path != NULL; i++)
        if (strcmp(zips[i].path, zip) == 0) { apk = zips[i].apk; break; }
    if (apk == NULL && i < UN_WWW_ZIPS) {
        apk = apk_open(zip);
        if (apk != NULL) { zips[i].path = strdup(zip); zips[i].apk = apk; }
    }
    ok = apk != NULL && apk_read_file(apk, entry, buf, len) == APK_OK;
    pthread_mutex_unlock(&un_www_zip_lock);
    return ok;
}

static int
un_www_read_file(const char *path, char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    *buf = malloc(n > 0 ? n : 1);
    *len = (n > 0 && fread(*buf, 1, n, f) == (size_t)n) ? (size_t)n : 0;
    fclose(f);
    return n >= 0;
}

static void *
un_www_run(void *arg)
{
    struct un_www_req *r = arg;
    JNIEnv *env = r->env;
    un_www_header_t header = (void *)jnienv_find_native_method(UN_WWW_CLASS, "headerCallback");
    un_www_read_t readcb = (void *)jnienv_find_native_method(UN_WWW_CLASS, "readCallback");
    un_www_progress_t progress = (void *)jnienv_find_native_method(UN_WWW_CLASS, "progressCallback");
    un_www_done_t done = (void *)jnienv_find_native_method(UN_WWW_CLASS, "doneCallback");
    un_www_error_t error = (void *)jnienv_find_native_method(UN_WWW_CLASS, "errorCallback");
    char *buf = NULL, err[1024];
    size_t len = 0;
    int found = 0;

    err[0] = '\0';
    if (strncmp(r->url, "jar:file://", 11) == 0) {
        char *zip = strdup(r->url + 11), *bang = strstr(zip, "!/");
        if (bang != NULL) {
            *bang = '\0';
            found = un_www_read_zip(zip, bang + 2, &buf, &len);
            if (!found)
                snprintf(err, sizeof(err), "java.io.FileNotFoundException: JAR entry %s not found in %s",
                         bang + 2, zip);
        } else {
            snprintf(err, sizeof(err), "java.net.MalformedURLException: no !/ in spec");
        }
        free(zip);
    } else if (strncmp(r->url, "file://", 7) == 0) {
        found = un_www_read_file(r->url + 7, &buf, &len);
        if (!found)
            snprintf(err, sizeof(err), "java.io.FileNotFoundException: %s (No such file or directory)",
                     r->url + 7);
    } else {
        /* No network on this path: what an offline Android device reports. */
        char host[256] = "";
        const char *h = strstr(r->url, "://");
        if (h != NULL) sscanf(h + 3, "%255[^/:?]", host);
        snprintf(err, sizeof(err), "java.net.UnknownHostException: Unable to resolve host \"%s\": "
                 "No address associated with hostname", host);
    }
    fprintf(stderr, "[UN-WWW] #%d %s -> %s\n", r->id, r->url,
            found ? "ok" : err);
    if (found) fprintf(stderr, "[UN-WWW] #%d %zu bytes\n", r->id, len);

    if (found && header != NULL && readcb != NULL && done != NULL) {
        char h[64];
        jbyteArray arr;
        size_t chunk = len < 0x8000 ? (len ? len : 0x8000) : 0x8000, off = 0;
        int aborted = 0;

        snprintf(h, sizeof(h), "content-length: %zu\n\r", len);
        if (header(env, un_www_class, r->id, (*env)->NewStringUTF(env, h))) {
            aborted = 1;
        } else {
            arr = (*env)->NewByteArray(env, (jsize)chunk);
            /* WWW.run(): the loop starts with read count 0. */
            aborted = readcb(env, un_www_class, r->id, arr, 0);
            while (!aborted && off < len) {
                size_t n = len - off < chunk ? len - off : chunk;
                (*env)->SetByteArrayRegion(env, arr, 0, (jsize)n, (jbyte *)buf + off);
                off += n;
                aborted = readcb(env, un_www_class, r->id, arr, (jint)n);
                if (!aborted && progress != NULL)
                    progress(env, un_www_class, r->id, 1.0f, (jfloat)off / (jfloat)len, 0.0, (jint)len);
            }
        }
        if (aborted) {
            char m[1024];
            snprintf(m, sizeof(m), "%s aborted", r->url);
            if (error != NULL) error(env, un_www_class, r->id, (*env)->NewStringUTF(env, m));
        } else {
            if (progress != NULL)
                progress(env, un_www_class, r->id, 1.0f, 1.0f, 0.0, (jint)len);
            done(env, un_www_class, r->id);
        }
    } else if (error != NULL) {
        error(env, un_www_class, r->id, (*env)->NewStringUTF(env, err));
    }
    free(buf);
    free(r->url);
    free(r);
    return NULL;
}

/* new WWW(int id, String url, byte[] postData, Map headers): starts the thread. */
static jobject
un_www_new(JNIEnv *env, jclass cls, struct un_args *a)
{
    struct un_www_req *r = calloc(1, sizeof(*r));
    const char *url;
    pthread_t t;
    pthread_attr_t attr;

    r->env = env;
    r->id = (int)(a->jv != NULL ? a->jv[a->i++].i : va_arg(*a->ap, jint));
    url = un_jstr(env, un_arg_ptr(a));
    r->url = strdup(url != NULL ? url : "");
    struct un_www_obj *o = calloc(1, sizeof(*o));
    o->clazz = un_class(UN_WWW_CLASS);
    o->magic = UN_WWW_MAGIC;
    (void)t; (void)attr;
    pthread_create(&o->thread, NULL, un_www_run, r);
    return o;
}

/* The "..." form. Before Unity 4.2 this was jni/jnienv.c's (returns NULL). */
static jobject
unity_jnienv_NewObject_gated(JNIEnv *env, jclass cls, jmethodID m, ...)
{
    jobject o;
    va_list ap;
    if (!un_unity42)
        return NULL;
    va_start(ap, m);
    o = unity_jnienv_NewObjectV(env, cls, m, ap);
    va_end(ap);
    return o;
}

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
/* UnityPlayer.onSurfaceCreated() -> nativeRecreateGfxState(). This is where the
 * real host builds the graphics device, with the EGL context the Java side just
 * created already current - i.e. it is the call that decides ES1 vs ES2. */
typedef void (*unity_nativeRecreateGfxState_t)(JNIEnv*, jobject);  /* ()V  */
/* com.unity3d.player.p.onSensorChanged() -> queueEvent -> p$1.run() ->
 * UnityPlayer.nativeSensor(x, y, z, timestampNanos). This is how acceleration
 * reaches Input.acceleration; there is no polling path on the native side. */
typedef void (*unity_nativeSensor_t)(JNIEnv*, jobject, jfloat, jfloat, jfloat, jlong) SOFTFP;
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
    unity_nativeRecreateGfxState_t nativeRecreateGfxState;
    unity_nativeSensor_t nativeSensor;
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
    if (un_portrait()) {
        GLOBAL_M->module_hacks->render_to_fbo = 1;
        GLOBAL_M->module_hacks->fbo_w = 768;
        GLOBAL_M->module_hacks->fbo_h = 1024;
    }

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
    /* Unity 4 AndroidJavaObject bridge (inert unless the host is Unity 4). */
    self->override_env.FromReflectedMethod = unity_jnienv_FromReflectedMethod;
    self->override_env.FromReflectedField = unity_jnienv_FromReflectedField;
    self->override_env.NewObjectA = unity_jnienv_NewObjectA;
    self->override_env.NewObjectV = unity_jnienv_NewObjectV;
    self->override_env.CallObjectMethodA = unity_jnienv_CallObjectMethodA;
    self->override_env.CallVoidMethodA = unity_jnienv_CallVoidMethodA;
    self->override_env.GetIntField = unity_jnienv_GetIntField;
    self->override_env.GetFloatField = unity_jnienv_GetFloatField;
    /* Unity 4.2: jvalue[] forms (inert unless un_unity42). */
    self->override_env.CallStaticObjectMethodA = unity_jnienv_CallStaticObjectMethodA;
    self->override_env.CallStaticVoidMethodA = unity_jnienv_CallStaticVoidMethodA;
    self->override_env.CallStaticBooleanMethodA = unity_jnienv_CallStaticBooleanMethodA;
    self->override_env.CallStaticIntMethodA = unity_jnienv_CallStaticIntMethodA;
    self->override_env.CallStaticLongMethodA = unity_jnienv_CallStaticLongMethodA;
    self->override_env.CallStaticFloatMethodA = unity_jnienv_CallStaticFloatMethodA;
    self->override_env.CallStaticDoubleMethodA = unity_jnienv_CallStaticDoubleMethodA;
    self->override_env.CallBooleanMethodA = unity_jnienv_CallBooleanMethodA;
    self->override_env.CallIntMethodA = unity_jnienv_CallIntMethodA;
    self->override_env.CallLongMethodA = unity_jnienv_CallLongMethodA;
    self->override_env.CallFloatMethodA = unity_jnienv_CallFloatMethodA;
    self->override_env.CallDoubleMethodA = unity_jnienv_CallDoubleMethodA;
    self->override_env.GetObjectField = unity_jnienv_GetObjectField;
    self->override_env.GetObjectArrayElement = unity_jnienv_GetObjectArrayElement;
    self->override_env.NewObject = unity_jnienv_NewObject_gated;

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


/* ---- force Unity's GLES2 graphics device ----------------------------------
 * Temple Run 2 ships 8 shaders whose ONLY subprograms are GLSL ES 2.0 (no
 * fixed-function pass, no Fallback), so on Unity's fixed-function device they
 * draw with the magenta error material. The engine picks the device here:
 *
 *   libunity+0x2d2f68  ldr  r3, [pc, #44]      ; -> global byte at +0x699de4
 *   libunity+0x2d2f6c  add  r3, pc, r3
 *   libunity+0x2d2f70  ldrb r3, [r3]
 *   libunity+0x2d2f74  cmp  r3, #0
 *   libunity+0x2d2f78  bne  +0x2d2f88          ; != 0 -> ES2 device init (+0x2c44ac)
 *   libunity+0x2d2f7c  bl   +0x2b69b4          ; == 0 -> ES1 device init  <-- ours
 *
 * Found by instrumenting, not by reading: a one-shot stack scan at the first
 * glGetString gave the chain +0x2b5ed0 (ES1 caps) <- +0x2b69c4 (ES1 device
 * init) <- +0x2d2f80, and that frame's disassembly is the branch above. The
 * two "Creating OpenGLES*.x graphics device" strings in libunity have NO code
 * references (dead strings in a release build), which is why grepping for them
 * found nothing.
 *
 * We set the flag AND force the branch: the flag so every other flag-dependent
 * code path agrees, the branch so a later write to the flag cannot undo it.
 * Only ever done when the live context really is ES2 - forcing the ES2 device
 * onto an ES1 context would break everything. APKENV_UNITY_GLES2=0 disables.
 *
 * The offsets are build-specific, so every patched word is verified against its
 * expected value first and the patch is skipped (loudly) on any mismatch. */
#define UN_GLES2_BRANCH_OFF   0x2d2f78
#define UN_GLES2_LDRB_OFF     0x2d2f70
#define UN_GLES2_CMP_OFF      0x2d2f74
#define UN_GLES2_FLAG_OFF     0x699de4
#define UN_GLES2_BRANCH_ORIG  0x1a000002u   /* bne */
#define UN_GLES2_BRANCH_NEW   0xea000002u   /* b   */
#define UN_GLES2_LDRB_ORIG    0xe5d33000u
#define UN_GLES2_CMP_ORIG     0xe3530000u

static void
un_force_gles2_device(void *addr_in_libunity)
{
    struct { const char *dli_fname; void *dli_fbase;
             const char *dli_sname; void *dli_saddr; } info;
    const char *env = getenv("APKENV_UNITY_GLES2");
    unsigned char *base;
    unsigned int *branch;
    unsigned char *flag;
    long pagesize = sysconf(_SC_PAGESIZE);

    if (env != NULL && env[0] == '0') {
        fprintf(stderr, "[UN-GLES2] disabled by APKENV_UNITY_GLES2=0\n");
        return;
    }
    if (apkenv_active_gles_version() != 2) {
        fprintf(stderr, "[UN-GLES2] live context is ES1 - leaving the engine on its "
                        "fixed-function device (forcing ES2 here would draw nothing)\n");
        return;
    }
    memset(&info, 0, sizeof(info));
    if (addr_in_libunity == NULL ||
        !apkenv_android_dladdr(addr_in_libunity, &info) || info.dli_fbase == NULL) {
        fprintf(stderr, "[UN-GLES2] cannot locate libunity's base - not patching\n");
        return;
    }
    base = (unsigned char *)info.dli_fbase;
    branch = (unsigned int *)(base + UN_GLES2_BRANCH_OFF);
    flag = base + UN_GLES2_FLAG_OFF;

    if (*(unsigned int *)(base + UN_GLES2_LDRB_OFF) != UN_GLES2_LDRB_ORIG ||
        *(unsigned int *)(base + UN_GLES2_CMP_OFF) != UN_GLES2_CMP_ORIG ||
        *branch != UN_GLES2_BRANCH_ORIG) {
        fprintf(stderr, "[UN-GLES2] libunity does not match the expected build "
                        "(%08x/%08x/%08x) - NOT patching\n",
                *(unsigned int *)(base + UN_GLES2_LDRB_OFF),
                *(unsigned int *)(base + UN_GLES2_CMP_OFF), *branch);
        return;
    }

    fprintf(stderr, "[UN-GLES2] libunity base=%p, device-select flag was %d\n",
            (void *)base, (int)*flag);
    if (*flag != 0) {
        fprintf(stderr, "[UN-GLES2] flag already set by the engine - no patch needed\n");
        return;
    }

    /* the flag: make every flag-dependent path agree */
    if (mprotect((void *)((unsigned long)flag & ~(unsigned long)(pagesize - 1)),
                 pagesize, PROT_READ | PROT_WRITE) == 0)
        *flag = 1;
    else
        fprintf(stderr, "[UN-GLES2] mprotect(flag) failed: %s\n", strerror(errno));

    /* the branch: bne -> b, so the ES2 device is chosen regardless */
    if (mprotect((void *)((unsigned long)branch & ~(unsigned long)(pagesize - 1)),
                 pagesize, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        *branch = UN_GLES2_BRANCH_NEW;
        __builtin___clear_cache((char *)branch, (char *)branch + 4);
        fprintf(stderr, "[UN-GLES2] forced the ES2 graphics device "
                        "(flag=1, +0x%x bne -> b)\n", UN_GLES2_BRANCH_OFF);
    } else {
        fprintf(stderr, "[UN-GLES2] mprotect(branch) failed: %s\n", strerror(errno));
    }
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
    self->priv->nativeRecreateGfxState = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeRecreateGfxState");
    self->priv->nativeSensor = jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeSensor");
    fprintf(stderr, "[UN] natives: init=%p file=%p render=%p initJni=%p androidInit=%p prepare=%p touch=%p prefs=%p\n",
            (void*)self->priv->nativeInit, (void*)self->priv->nativeFile, (void*)self->priv->nativeRender,
            (void*)self->priv->initJni, (void*)self->priv->unityAndroidInit, (void*)self->priv->unityAndroidPrepareGameLoop,
            (void*)self->priv->nativeTouch, (void*)self->priv->InitPlayerPrefs);

    /* Java UnityPlayer order: <init>: nativeFile(apk); a(): initJni, PlayerPrefs, nativeInitWWW;
     * GL thread: nativeInit(w,h); first onDrawFrame: unityAndroidInit("assets/bin/", dataDir/lib),
     * unityAndroidPrepareGameLoop; then nativeRender() per frame. */
    un_unity4 = (jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeSetInputCanceled") != NULL);
    fprintf(stderr, "[UN] unity4 host: %s\n", un_unity4 ? "yes" : "no (3.5 order)");
    un_unity42 = un_unity4 &&
        jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeSetDefaultDisplay") != NULL;
    fprintf(stderr, "[UN] unity 4.2 host (jvalue[] AndroidJavaObject calls): %s\n",
            un_unity42 ? "yes" : "no");
    if (un_unity42) {
        struct un_pref *rc = un_pref_find(UN_GLU_RUNCOUNT_KEY);
        int prev = (rc != NULL && rc->type == UN_PREF_INT) ? rc->i : 0;
        un_glu_runcount = prev + 1;
        rc = un_pref_slot(UN_GLU_RUNCOUNT_KEY);
        if (rc != NULL) {
            rc->type = UN_PREF_INT; rc->i = un_glu_runcount; un_prefs_dirty = 1;
            un_prefs_save();
        }
        fprintf(stderr, "[UN] launch #%d (host run count)\n", un_glu_runcount);
    }
    {
        const char *pkg = getenv("APKENV_UNITY_PACKAGE");
        if (pkg != NULL && pkg[0] != '\0') un_pkg = pkg;
        fprintf(stderr, "[UN] package name: %s\n", un_pkg);
    }

    /* APKENV_UNITY_OBB_CODEPATH=1: Glu's UnityLauncherActivity overrides
     * getPackageCodePath() to return the OBB, so the engine's ONLY nativeFile()
     * is the expansion file and Application.dataPath points into it. RoboCop's
     * offline content fallbacks (jar:file://<dataPath>!/assets/ABTesting.xml,
     * .../assets/AssetBundles/) exist only in the OBB; with the apk as dataPath
     * DynamicContentPipeline failed ("SLOW INTERNET CONNECTION", robocop-r6). */
    un_obb_codepath = getenv("APKENV_UNITY_OBB_CODEPATH") != NULL &&
                      getenv("APKENV_UNITY_OBB_CODEPATH")[0] == '1';
    if (!un_obb_codepath) {
        jstring file = GLOBAL_M->env->NewStringUTF(ENV_M,global->apk_filename);
        un_hookcheck("nativeFile"); fprintf(stderr, "[UN] nativeFile(%s)\n", global->apk_filename);
        self->priv->nativeFile(ENV_M,GLOBAL_M,file);
    }
    /* Split-binary games (settings.xml useObb=True) keep assets/bin/Data in an
     * expansion file. The Unity 4 host's constructor follows nativeFile(apk)
     * with l(), which calls nativeFile() once more for each OBB it finds under
     * <sdcard>/Android/obb/<pkg>/. We are the host: APKENV_UNITY_OBB names the
     * file (run-dir relative in a package), made absolute because the engine
     * opens it long after startup. */
    {
        const char *obb = getenv("APKENV_UNITY_OBB");
        if (obb != NULL && obb[0] != '\0') {
            char abs[PATH_MAX];
            struct stat st;
            if (realpath(obb, abs) != NULL && stat(abs, &st) == 0) {
                if (un_obb_codepath) un_codepath = strdup(abs);
                fprintf(stderr, "[UN] nativeFile(obb %s, %lld bytes)%s\n", abs, (long long)st.st_size,
                        un_obb_codepath ? " [as the package code path]" : "");
                self->priv->nativeFile(ENV_M, GLOBAL_M, GLOBAL_M->env->NewStringUTF(ENV_M, abs));
            } else {
                fprintf(stderr, "[UN] WARNING: APKENV_UNITY_OBB=%s not found (%s) - the engine "
                                "will miss every asset in the expansion file\n", obb, strerror(errno));
            }
        }
    }
    if (self->priv->initJni) { un_hookcheck("initJni"); fprintf(stderr, "[UN] initJni\n"); self->priv->initJni(ENV_M,GLOBAL_M); }
    if (self->priv->InitPlayerPrefs) { un_hookcheck("InitPlayerPrefs"); fprintf(stderr, "[UN] InitPlayerPrefs\n"); self->priv->InitPlayerPrefs(ENV_M,GLOBAL_M); }
    /* Java host a(IZ): nativeInitWWW(WWW.class) right after PlayerPrefs. */
    if (un_unity42) {
        void (*initwww)(JNIEnv *, jobject, jclass) = (void *)
            jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeInitWWW");
        un_www_class = GLOBAL_M->env->FindClass(ENV_M, UN_WWW_CLASS);
        fprintf(stderr, "[UN] nativeInitWWW(%s)%s\n", UN_WWW_CLASS, initwww ? "" : " - NOT FOUND");
        if (initwww) initwww(ENV_M, GLOBAL_M, un_www_class);
    }
    /* Swap to the portrait surface the engine will believe in. Everything
     * downstream (nativeInit, nativeResize, the touch mapping) must use the
     * SAME pair, or the engine lays out for one size and receives input in
     * another. */
    if (un_portrait()) {
        width  = GLOBAL_M->module_hacks->fbo_w;
        height = GLOBAL_M->module_hacks->fbo_h;
        fprintf(stderr, "[UN] portrait: presenting a %dx%d surface to the engine\n",
                width, height);
    }
    un_screen_w = width; un_screen_h = height;
    /* nativeInit(II) is NOT (width, height). The Java host queues
     * UnityPlayer$24(glesMode, splashMode) from a(IZ): glesMode is init(IZ)'s
     * argument (settings.xml gles_mode = 2) and splashMode is
     * getSettings().getInt("splash_mode") (= 1). We passed the screen size
     * here for the whole port - which fed the engine glesMode=1024 (not 2, so
     * it built its fixed-function device) and a splash mode of 768 (so the
     * Imangi logo screen never showed). The size reaches the engine through
     * nativeResize, which we already call correctly. */
    /* Unity 4.2 host a(IZ): nativeSetDefaultDisplay(getWindowManager()
     * .getDefaultDisplay()) just before queueing nativeInit. */
    if (un_unity42) {
        void (*setdisp)(JNIEnv *, jobject, jobject) = (void *)
            jnienv_find_native_method(UNITYPLAYER_CLASS_NAME, "nativeSetDefaultDisplay");
        fprintf(stderr, "[UN] nativeSetDefaultDisplay(Display %dx%d)\n", width, height);
        setdisp(ENV_M, GLOBAL_M, un_new_obj("android/view/Display", NULL));
    }
    {
        int gles_mode = 2, splash_mode = 1;
        const char *e;
        if ((e = getenv("APKENV_UNITY_GLES_MODE")) != NULL) gles_mode = atoi(e);
        if ((e = getenv("APKENV_UNITY_SPLASH")) != NULL) splash_mode = atoi(e);
        un_hookcheck("nativeInit");
        fprintf(stderr, "[UN] nativeInit(glesMode=%d, splashMode=%d)  [surface %dx%d via nativeResize]\n",
                gles_mode, splash_mode, width, height);
        self->priv->nativeInit(ENV_M,GLOBAL_M,gles_mode,splash_mode);
    }
    /* After nativeInit (which may set the device-select flag itself now that it
     * receives glesMode=2) and before unityAndroidInit (which creates the
     * device). The patch skips itself if the flag is already set. */
    un_force_gles2_device((void *)self->priv->nativeInit);
    /* onSurfaceCreated -> nativeRecreateGfxState, which the Java host runs on the
     * GL thread BEFORE the first onDrawFrame. We had skipped it entirely, and
     * skipping it is a candidate cause of the engine building its fixed-function
     * ES1 device while an ES2 context is current (measured: glMatrixMode /
     * glTexEnvf / glEnableClientState, never glCreateShader). */
    if (self->priv->nativeRecreateGfxState) {
        fprintf(stderr, "[UN] nativeRecreateGfxState (onSurfaceCreated)\n");
        self->priv->nativeRecreateGfxState(ENV_M,GLOBAL_M);
        fprintf(stderr, "[UN] nativeRecreateGfxState done\n");
    } else {
        fprintf(stderr, "[UN] nativeRecreateGfxState NOT FOUND\n");
    }

    /* Unity 4: GLSurfaceView runs onSurfaceChanged -> nativeResize(w, h, viewW,
     * viewH) and then the first onDrawFrame, which calls nativeRender() BEFORE
     * the host's one-time i() (unityAndroidInit -> nativeResize ->
     * unityAndroidPrepareGameLoop). That early resize is more than a size:
     * while the engine is not yet initialised, nativeResize (libunity+0x37a75c)
     * tail-calls +0x37378c, which lazily builds the loading-screen blitter
     * (glGenTextures + a shader; pointer at +0x78883c). Once initialised it only
     * updates scale factors. unityAndroidPrepareGameLoop's one-time setup
     * (+0x37a7ec) dereferences that pointer without a check, so without this
     * resize it is NULL -> SIGSEGV at +0x37a914 (Aralon run A2). Offsets are
     * Aralon's build and only document the path; nothing here depends on them. */
    if (un_unity4 && self->priv->nativeResize) {
        fprintf(stderr, "[UN] nativeResize(%d,%d,%d,%d) [unity4: onSurfaceChanged, pre-init]\n",
                width, height, width, height);
        self->priv->nativeResize(ENV_M,GLOBAL_M,width,height,width,height);
        if (self->priv->nativeRender) {
            jboolean r = self->priv->nativeRender(ENV_M,GLOBAL_M);
            fprintf(stderr, "[UN] nativeRender (pre-init, head of first onDrawFrame) -> %d\n", (int)r);
        }
    }

    /* Portrait: take the offscreen FBO now - AFTER the graphics device exists
     * (nativeInit + nativeRecreateGfxState) and BEFORE unityAndroidInit.
     * Creating it before the device produced GL_OUT_OF_MEMORY and a flat-colour
     * screen; creating it lazily on the engine's first glBindFramebuffer(0) let
     * the first frames (the 3D backdrop, unrotated) go straight to the window.
     * (An older comment here said unityAndroidInit "draws the splash". It does
     * not - the engine never opens splash.png at all; see compat/fbo_es2.c.) */
    if (un_portrait()) {
        GLuint fb = apkenv_fbo_es2_ensure();
        fprintf(stderr, "[UN] portrait FBO bound after device creation: %u\n", fb);
    }

    /* Put the boot splash on screen before the long blocking init.
     * unityAndroidInit does not return for seconds and nothing presents while
     * it runs, so the panel simply holds the last swapped buffer for the whole
     * load - which is exactly what makes a splash worth drawing here. Present
     * twice: with double buffering one swap leaves the OTHER buffer black, and
     * whatever swaps next would flash it. Both are no-ops unless
     * APKENV_SPLASH_RGB is set. */
    if (global->platform != NULL && global->platform->update != NULL) {
        global->platform->update();
        global->platform->update();
    }

    char libdir[512]; snprintf(libdir, sizeof(libdir), "%slib", home);
    jstring bin = GLOBAL_M->env->NewStringUTF(ENV_M,"assets/bin/");
    jstring lib = GLOBAL_M->env->NewStringUTF(ENV_M,libdir);
    un_main_tid = (pid_t)syscall(SYS_gettid);
    { pthread_t wt; pthread_create(&wt, NULL, un_hook_watch, NULL); }
    un_hookcheck("unityAndroidInitassets"); fprintf(stderr, "[UN] unityAndroidInit(assets/bin/, %s)\n", libdir);
    {
        /* (Z) in the Unity 4 host, (V) in 3.5 - where r0 is whatever was left. */
        jboolean ok = self->priv->unityAndroidInit(ENV_M,GLOBAL_M,bin,lib);
        if (un_unity4) fprintf(stderr, "[UN] unityAndroidInit done -> %d\n", (int)ok);
        else           fprintf(stderr, "[UN] unityAndroidInit done\n");
    }
    /* UnityPlayer.onDrawFrame()'s first-time tail, in the Java host's exact
     * order (read from each host's UnityPlayer.smali):
     *   3.5 (TR2):   unityAndroidInit -> unityAndroidPrepareGameLoop -> nativeResize
     *   4.0 (i()):   unityAndroidInit -> nativeResize -> unityAndroidPrepareGameLoop
     *   both then:   nativeResume() -> windowFocusChanged(true)
     * Skipping resize/resume leaves the engine sized 0x0 AND paused: it clears
     * to the camera background every frame and submits no geometry at all
     * (measured on TR2: 0 glDrawArrays, glViewport never called). On 3.5 the
     * resize must come AFTER prepareGameLoop - next to nativeInit is too early,
     * the graphics device does not exist yet and the size is dropped. */
    if (un_unity4 && self->priv->nativeResize) {
        fprintf(stderr, "[UN] nativeResize(%d,%d,%d,%d) [unity4: before prepare]\n", width, height, width, height);
        self->priv->nativeResize(ENV_M,GLOBAL_M,width,height,width,height);
    }
    if (self->priv->unityAndroidPrepareGameLoop) {
        fprintf(stderr, "[UN] unityAndroidPrepareGameLoop\n");
        self->priv->unityAndroidPrepareGameLoop(ENV_M,GLOBAL_M);
        fprintf(stderr, "[UN] prepare done\n");
    }
    if (!self->priv->nativeResize) {
        fprintf(stderr, "[UN] WARNING: nativeResize not found - expect a blank screen\n");
    } else if (!un_unity4) {
        fprintf(stderr, "[UN] nativeResize(%d,%d,%d,%d)\n", width, height, width, height);
        self->priv->nativeResize(ENV_M,GLOBAL_M,width,height,width,height);
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

    /* Rotate the touch into the engine's portrait surface. SDL reports real
     * panel pixels (landscape); the engine believes it owns a 768x1024 surface
     * that we rotate at present time, so input must travel the same rotation or
     * the player taps one place and the game reacts somewhere else.
     *
     * Derived from the present quad's texcoords rather than by trial: for the
     * 90-degree case screen-bottom-left samples the FBO's top-left, which
     * gives u = 1 - sy/sh and v = 1 - sx/sw, and the engine's y runs downward
     * from the top while GL's v runs up. */
    if (un_portrait()) {
        int sw = 0, sh = 0;
        int fw = GLOBAL_M->module_hacks->fbo_w, fh = GLOBAL_M->module_hacks->fbo_h;
        GLOBAL_M->platform->get_size(&sw, &sh);
        if (sw > 0 && sh > 0 && fw > 0 && fh > 0) {
            int px, py;
            if (apkenv_fbo_es2_rotation() == 3) {          /* 270 degrees */
                px = (int)((float)fw * (float)y / (float)sh);
                py = (int)((float)fh * (1.0f - (float)x / (float)sw));
            } else {                                        /* 90 degrees */
                px = (int)((float)fw * (1.0f - (float)y / (float)sh));
                py = (int)((float)fh * ((float)x / (float)sw));
            }
            if (px < 0) px = 0; else if (px > fw - 1) px = fw - 1;
            if (py < 0) py = 0; else if (py > fh - 1) py = fh - 1;
            x = px; y = py;
        }
    }
    jlong t = un_now_ms();
    static int logged = 0;
    /* Bounded: this runs for the whole session and writes to /media/internal. */
    if (logged < 200) {
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

/* Scripted taps, for verifying gameplay without a human at the device:
 *   APKENV_UNITY_AUTOTAP="frame:x:y[,frame:x:y...]"
 * Injects a DOWN at <frame> and an UP six frames later, through exactly the
 * same path a real touch takes. Diagnostic only; unset by default. */
static void
un_autotap(struct SupportModule *self, unsigned long frame)
{
    const char *spec = getenv("APKENV_UNITY_AUTOTAP");
    const char *p;
    if (spec == NULL || spec[0] == 0) return;
    for (p = spec; *p; ) {
        unsigned long f = strtoul(p, (char **)&p, 10);
        int x = 0, y = 0;
        if (*p == ':') { x = (int)strtol(p + 1, (char **)&p, 10); }
        if (*p == ':') { y = (int)strtol(p + 1, (char **)&p, 10); }
        if (frame == f || frame == f + 6) {
            int down = (frame == f);
            fprintf(stderr, "[UN-AUTOTAP] frame %lu: %s at %d,%d\n",
                    frame, down ? "DOWN" : "UP", x, y);
            unity_input(self, down ? ACTION_DOWN : ACTION_UP, x, y, 0);
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}


/* ---- tilt ------------------------------------------------------------------
 * Parts of Temple Run 2 steer by tilting the device, and nothing was feeding
 * the engine acceleration: apkenv's accelerometer went to SDL's joystick API,
 * which finds nothing on webOS (no joydev), and the module never called
 * nativeSensor even when it did.
 *
 * The Java host's own transform, read from com.unity3d.player.p.onSensorChanged:
 *
 *   row = (Display.getOrientation() - 1) & 3          // we answer 0 -> row 3
 *   d[] = { 1, 1,0,1,   -1, 1,1,0,   -1,-1,0,1,   1,-1,1,0 }
 *   K   = -1/9.80665                                  // m/s2 -> g, and negated
 *   x' = d[row*4+0] * K * v[d[row*4+2]]
 *   y' = d[row*4+1] * K * v[d[row*4+3]]
 *   z' =                 K * v[2]
 *
 * i.e. for row 3:  x' = K*v[1],  y' = -K*v[0],  z' = K*v[2].
 * Doing the remap here (rather than feeding raw values) keeps us bit-identical
 * to what the real host would have sent for the orientation we report.
 *
 * Timestamp is SensorEvent.timestamp: nanoseconds on a monotonic clock. */
static void
un_feed_tilt(struct SupportModule *self)
{
    static const int d[16] = { 1, 1, 0, 1,  -1, 1, 1, 0,  -1, -1, 0, 1,  1, -1, 1, 0 };
    const float K = -1.0f / 9.80665f;
    static int row = -1, invx, invy, loaded;
    float v[3] = { 0.0f, 0.0f, 0.0f };
    struct timespec ts;
    int ix, iy;
    float x, y, z;

    if (self->priv->nativeSensor == NULL)
        return;

    /* Calibration knobs. The device frame is settled by physically tilting, so
     * make that loop cheap: as well as the env vars, re-read an optional file on
     * /media/internal (writable, unlike the app dir) so the mapping can be
     * changed on the device without repackaging - just relaunch.
     *
     *   row = which orientation row of the Java host's table to apply (0..3)
     *   invx/invy = flip the steering / pitch axis after the remap
     *
     * File format, one per line: "row=0", "invx=1", "invy=0". */
    if (!loaded) {
        const char *e;
        FILE *f;
        loaded = 1;
        /* Confirmed on device (2026-08-27): row 0 + inverted Y is what makes
         * Temple Run 2 steer correctly. The game is portrait-built (the manifest
         * declares portrait on every Unity activity) while we present a
         * landscape surface, so its steering axis sits a quarter-turn from the
         * one the reported orientation implies: with row 3 the roll landed on
         * Input.acceleration.x while the axis the game actually reads carried
         * gravity - a constant value, i.e. a constant drift, whose direction
         * flipped with how the tablet was held. Row 0 puts the roll on Y and
         * parks gravity on X; invy fixes the mirror. */
        /* The sensor frame rotates with the device, so the tilt mapping is
         * coupled to BOTH the presented orientation and which way we rotate the
         * panel (APKENV_FBO_ROT).
         *
         * Landscape: row 0 + inverted Y, measured on device - the inversion was
         * compensating for the game being portrait-built while held landscape.
         * Portrait: the device is now held the way the game was designed for,
         * so the host's natural portrait mapping should apply without the
         * compensation. Verify by tilting; /media/internal/apkenv-tilt.conf
         * overrides both without a rebuild. */
        if (un_portrait()) {
            /* Confirmed on device: holding the tablet turned for portrait moves
             * the roll from the sensor's Y to its X, so the steering axis has to
             * come from v[0] - rows 1 and 3 - and row 3 + inverted Y is the one
             * that steers the right way. If the panel is rotated the other way
             * (APKENV_FBO_ROT=3) the player holds the tablet the other way up,
             * which mirrors it again. */
            row = 3; invx = 0; invy = 1;
            if (apkenv_fbo_es2_rotation() == 3) invy = 0;
        } else {
            row = 0; invx = 0; invy = 1;
        }
        if ((e = getenv("APKENV_UNITY_TILT_ROW")) != NULL) row = atoi(e) & 3;
        if ((e = getenv("APKENV_UNITY_TILT_INVX")) != NULL) invx = (*e != '0');
        if ((e = getenv("APKENV_UNITY_TILT_INVY")) != NULL) invy = (*e != '0');
        f = fopen("/media/internal/apkenv-tilt.conf", "r");
        if (f != NULL) {
            char line[128];
            while (fgets(line, sizeof(line), f) != NULL) {
                int val;
                if (sscanf(line, " row = %d", &val) == 1) row = val & 3;
                else if (sscanf(line, " invx = %d", &val) == 1) invx = (val != 0);
                else if (sscanf(line, " invy = %d", &val) == 1) invy = (val != 0);
            }
            fclose(f);
            fprintf(stderr, "[UN-TILT] /media/internal/apkenv-tilt.conf applied\n");
        }
        fprintf(stderr, "[UN-TILT] row=%d invx=%d invy=%d (portrait=%d panel-rot=%d)\n",
                row, invx, invy, un_portrait(), apkenv_fbo_es2_rotation());
    }

    if (!apkenv_accelerometer_get(&v[0], &v[1], &v[2]))
        return;

    ix = d[row * 4 + 2];
    iy = d[row * 4 + 3];
    x = (float)d[row * 4 + 0] * K * v[ix];
    y = (float)d[row * 4 + 1] * K * v[iy];
    z = K * v[2];
    if (invx) x = -x;
    if (invy) y = -y;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    self->priv->nativeSensor(ENV_M, GLOBAL_M, (jfloat)x, (jfloat)y, (jfloat)z,
                             (jlong)ts.tv_sec * 1000000000LL + ts.tv_nsec);

    if (getenv("APKENV_ACCEL_DEBUG") != NULL) {
        static int n;
        if (n++ % 30 == 0)
            fprintf(stderr, "[UN-TILT] accel m/s2=(% .2f,% .2f,% .2f) -> "
                            "Input.acceleration g=(% .3f,% .3f,% .3f)\n",
                    v[0], v[1], v[2], x, y, z);
    }
}

static void
unity_update(struct SupportModule *self)
{
    { static unsigned long f; un_autotap(self, ++f); }
    un_feed_tilt(self);

    if (self->priv->nativeRender) {
        jboolean r = self->priv->nativeRender(ENV_M,GLOBAL_M);
        self->priv->frames++;
        if (self->priv->frames <= 3 || (self->priv->frames % 600) == 0) {
            /* Frame rate over the last 600 frames, from the monotonic clock. */
            static jlong t0;
            jlong t = un_now_ms();
            if ((self->priv->frames % 600) == 0 && t0 != 0)
                fprintf(stderr, "[UN] nativeRender #%lu -> %d  (%.1f fps)\n", self->priv->frames, r,
                        600000.0 / (double)(t - t0));
            else
                fprintf(stderr, "[UN] nativeRender #%lu -> %d\n", self->priv->frames, r);
            if ((self->priv->frames % 600) == 0 || self->priv->frames == 3) t0 = t;
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




