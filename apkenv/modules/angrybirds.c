
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
 * Angry Birds Space support module 0.6 - By: Arto Rusanen
 *
 **/

#include "common.h"
#include "../audio/audio.h"

#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

// Typedefs. Got these from classes.dex (http://stackoverflow.com/questions/1249973/decompiling-dex-into-java-sourcecode)
typedef jboolean (*angrybirds_init_t)(JNIEnv *env, jobject obj, jint paramInt1, jint paramInt2, jstring paramString) SOFTFP;
typedef jboolean (*angrybirds_resize_t)(JNIEnv *env, jobject obj, jint width, jint height) SOFTFP;
typedef void (*angrybirds_input_t)(JNIEnv *env, jobject obj, jint action, jfloat x, jfloat y, jint finger) SOFTFP;
typedef void (*angrybirds_keyinput_t)(JNIEnv *env, jobject obj, jint keycode, jint unicode, jint is_down) SOFTFP;
typedef jboolean (*angrybirds_update_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*angrybirds_pause_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*angrybirds_resume_t)(JNIEnv *env, jobject obj) SOFTFP;
typedef void (*angrybirds_mixdata_t)(JNIEnv *env, jobject obj, jlong paramLong, jbyteArray paramArrayOfByte, jint paramInt) SOFTFP;
typedef void (*angrybirds_deinit_t)(JNIEnv *env, jobject obj) SOFTFP;


struct SupportModulePriv {
    jni_onload_t JNI_OnLoad;
    angrybirds_init_t native_init;
    angrybirds_resize_t native_resize;
    angrybirds_update_t native_update;
    angrybirds_input_t native_input;
    angrybirds_keyinput_t native_keyinput;
    angrybirds_pause_t native_pause;
    angrybirds_resume_t native_resume;
    angrybirds_mixdata_t native_mixdata;
    angrybirds_deinit_t native_deinit;
    const char *myHome;
    int want_exit;
};
static struct SupportModulePriv angrybirds_priv;

/* Audio specs and handle */
jlong audioHandle;

/* Global application state so we can call this from override thingie */
static struct GlobalState *global;

/* --- engine->host call-out tracer (see PORTING-PLAYBOOK.md §3) -------------
 * Every Java method the engine calls that this module does not implement is a
 * potential contract gap; log each distinct one once with its signature. */
#define AB_TRACE_MAX 64
static struct { const char *name; unsigned long n; } ab_trace[AB_TRACE_MAX];
static int ab_trace_n = 0;
static void
ab_trace_unhandled(const char *kind, jmethodID method)
{
    int i;
    for (i = 0; i < ab_trace_n; i++)
        if (strcmp(ab_trace[i].name, method->name) == 0) {
            unsigned long n = ++ab_trace[i].n;
            if (n == 100 || n == 10000 || n == 1000000)
                fprintf(stderr, "[AB-JNI] %s %s called %lu times\n", kind, method->name, n);
            return;
        }
    fprintf(stderr, "[AB-JNI] UNHANDLED %s %s%s\n", kind, method->name, method->sig ? method->sig : "");
    if (ab_trace_n < AB_TRACE_MAX) {
        ab_trace[ab_trace_n].name = strdup(method->name);
        ab_trace[ab_trace_n].n = 1;
        ab_trace_n++;
    }
}

/* --- readFile contract (Rovio ka3d, Amazing Alex generation) ---------------
 * MyRenderer.readFile(name): if name doesn't end in ".zip", try "<name>.zip"
 * and return the FIRST ENTRY of that zip inflated; on failure return the raw
 * asset. Only a few big .pvr sheets are zipped, but a raw read would hand the
 * engine zip bytes instead of the texture. Minimal single-entry zip reader:
 * local file header (PK\3\4) + raw deflate via zlib. */
static struct dummy_array *
ab_unzip_first_entry(const char *zbuf, size_t zlen)
{
    if (zlen < 30 || memcmp(zbuf, "PK\003\004", 4) != 0) return NULL;
    const unsigned char *h = (const unsigned char *)zbuf;
    unsigned method = h[8] | (h[9] << 8);
    unsigned csize = h[18] | (h[19] << 8) | (h[20] << 16) | ((unsigned)h[21] << 24);
    unsigned usize = h[22] | (h[23] << 8) | (h[24] << 16) | ((unsigned)h[25] << 24);
    unsigned nlen = h[26] | (h[27] << 8), xlen = h[28] | (h[29] << 8);
    size_t off = 30 + nlen + xlen;
    if (off + csize > zlen || usize == 0) return NULL;
    char *out = malloc(usize);
    if (!out) return NULL;
    if (method == 0) {
        memcpy(out, zbuf + off, usize);
    } else if (method == 8) {
        z_stream zs; memset(&zs, 0, sizeof(zs));
        zs.next_in = (Bytef *)(zbuf + off); zs.avail_in = csize;
        zs.next_out = (Bytef *)out; zs.avail_out = usize;
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { free(out); return NULL; }
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) { free(out); return NULL; }
    } else { free(out); return NULL; }
    struct dummy_array *array = malloc(sizeof(*array));
    array->data = out; array->length = usize; array->element_size = 1;
    return array;
}

/* Fill audio buffer */
void my_audio_callback(void *ud, void *stream, int len)
{
    JNIEnv *thread_env;
    JNIEnv ref_env;

    jarray *array;
    (*VM(global))->AttachCurrentThread(VM(global), &thread_env, NULL);

    /* here we need the original NewGlobalRef */
    if(global->use_dvm)
    {
        ref_env = &global->dalvik_copy_env;
    }
    else ref_env = *thread_env;

    array = (*thread_env)->NewShortArray(thread_env, len / 2);

    jobject *ref = ref_env->NewGlobalRef(thread_env, array);
    angrybirds_priv.native_mixdata(ENV(global), VM(global), audioHandle, ref, len);
    ref_env->DeleteGlobalRef(thread_env, ref);

    jshort *elements = (*thread_env)->GetShortArrayElements(thread_env, array, 0);
    memcpy(stream, elements, len);
    (*thread_env)->ReleaseShortArrayElements(thread_env, array, elements, JNI_ABORT);

    (*thread_env)->DeleteLocalRef(thread_env, array);

    (*VM(global))->DetachCurrentThread(VM(global));
}


/* CallVoidMethodV override. Signal when to start or stop audio */
void
angrybirds_jnienv_CallVoidMethodV(JNIEnv* p0, jobject p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("module_angrybirds_jnienv_CallVoidMethodV(%x, %s, %s)\n", p1, p2->name, p2->sig);
    if (strcmp(p2->name, "startOutput") == 0)
    {
        fprintf(stderr, "[AB-AUDIO] startOutput\n");
        apkenv_audio_play();
    }
    else if (strcmp(p2->name, "stopOutput") == 0)
    {
        fprintf(stderr, "[AB-AUDIO] stopOutput\n");
        apkenv_audio_pause();
    }
    else
        ab_trace_unhandled("void", p2);
}

/* NewObjectV override. Initialize audio output */
jobject
angrybirds_jnienv_NewObjectV(JNIEnv *env, jclass p1, jmethodID p2, va_list p3)
{
    struct dummy_jclass *clazz = p1;
    MODULE_DEBUG_PRINTF("module_angrybirds_jnienv_NewObjectV(%x, %s, %s)\n", p1, p2->name, clazz->name);
    if (strcmp(clazz->name, "com/rovio/ka3d/AudioOutput") == 0)
    {
        /* Open the audio device */
        audioHandle = va_arg(p3, jlong);

        int freq = va_arg(p3, int);
        enum AudioFormat format = AudioFormat_S16SYS;
        int channels = va_arg(p3, int);
        jint bitrate = va_arg(p3, int);
        int samples = va_arg(p3, int) / 8;

        fprintf(stderr, "[AB-AUDIO] AudioOutput(freq=%d ch=%d bits=%d samples=%d)\n", freq, channels, bitrate, samples);
        apkenv_audio_open(freq, format, channels, samples, my_audio_callback, NULL);
    }

    if (global->use_dvm) {
        return (jobject)0x1;
    }

    return GLOBAL_J(env);
}

/* CallObjectMethodV override. AB calls readFile to read data from apk */
jobject
angrybirds_jnienv_CallObjectMethodV(JNIEnv *env, jobject p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("module_angrybirds_jnienv_CallObjectMethodV(%x, %s, %s, ...)\n", p1, p2->name, p2->sig);
    if (strcmp(p2->name, "readFile") == 0)
    {
        // Process input to prevent "not responding" message when game starts
        global->platform->input_update(global->active_module);

        char *str_data = dup_jstring(global, va_arg(p3, jstring*));
        char tmp[PATH_MAX];
        strcpy(tmp, "assets/");
        strcat(tmp, str_data);

        /* ka3d readFile contract: prefer "<name>.zip" (first entry, inflated) */
        {
            size_t l = strlen(str_data);
            char ztmp[PATH_MAX];
            if (l > 4 && strcmp(str_data + l - 4, ".zip") == 0) strcpy(ztmp, tmp);
            else snprintf(ztmp, sizeof(ztmp), "%s.zip", tmp);
            char *zbuf = NULL; size_t zlen = 0;
            if (apk_read_file(global->apklib_handle, ztmp, &zbuf, &zlen) == APK_OK) {
                struct dummy_array *arr = ab_unzip_first_entry(zbuf, zlen);
                free(zbuf);
                if (arr) {
                    static int zlog = 0;
                    if (zlog++ < 12) fprintf(stderr, "[AB-FILE] %s (zip -> %d bytes)\n", str_data, (int)arr->length);
                    free(str_data);
                    return arr;
                }
                fprintf(stderr, "[AB-FILE] %s: zip present but unreadable, using raw\n", str_data);
            }
        }

#ifdef PANDORA
        if (strcmp(str_data,"data/bundleIndex.idx")==0) //ignore this file, it segfaults but the games still work afterwards
            return NULL;
#endif

        jarray *result = global->read_file_to_jni_array(tmp);

        { static int rlog = 0;
          if (rlog++ < 40 || result == NULL)
              fprintf(stderr, "[AB-FILE] %s -> %s\n", str_data, result ? "ok" : "MISSING"); }
        free(str_data);
        return result;
    }
    else if (strcmp(p2->name, "getUniqueIdHash") == 0 || strcmp(p2->name, "getUniqueId") == 0)
    {
        return (*env)->NewStringUTF(env, "");
    }
    ab_trace_unhandled("obj", p2);
    return NULL;
}

/* DeleteLocalRef override. Free some memory :) */
void
angrybirds_jnienv_DeleteLocalRef(JNIEnv* p0, jobject p1)
{
    if(!global->use_dvm)
    {
        MODULE_DEBUG_PRINTF("angrybirds_jnienv_DeleteLocalRef(%x)\n", p1);
        if (p1 == GLOBAL_J(p0) || p1 == NULL) {
            MODULE_DEBUG_PRINTF("WARNING: DeleteLocalRef on global\n");
            return;
        }
        free(p1);
    }
    else
    {
        global->dalvik_copy_env.DeleteLocalRef(p0, p1);
    }
}

jobjectArray
angrybirds_jnienv_NewObjectArray(JNIEnv* p0, jsize p1, jclass p2, jobject p3)
{
    MODULE_DEBUG_PRINTF("angrybirds_jnienv_NewObjectArray()\n");
    return NULL;
}

jobject
angrybirds_jnienv_CallStaticObjectMethodV(JNIEnv* p0, jclass p1, jmethodID p2, va_list p3)
{
    struct dummy_jclass *jcl = p1;
    MODULE_DEBUG_PRINTF("angrybirds_jnienv__CallStaticObjectMethodV(%s, %s/%s, ...)\n",
            jcl->name, p2->name, p2->sig);
    return NULL;
}

void
angrybirds_jnienv_CallStaticVoidMethodV(JNIEnv* p0, jclass p1, jmethodID p2, va_list p3)
{
    struct dummy_jclass *jcl = p1;
    MODULE_DEBUG_PRINTF("angrybirds_jnienv__CallStaticVoidMethodV(%s, %s/%s, ...)\n",
            jcl->name, p2->name, p2->sig);
}

jboolean
angrybirds_jnienv_CallBooleanMethodV(JNIEnv* p0, jobject p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("angrybirds_jnienv__CallBooleanMethodV(%p, %s/%s, ...)\n", p1, p2->name, p2->sig);
    if (strcmp(p2->name, "isSilentProfile") == 0) return 0;
    ab_trace_unhandled("bool", p2);
    return 0;
}

static int
angrybirds_try_init(struct SupportModule *self)
{
    self->priv->native_init = (angrybirds_init_t)LOOKUP_M("ka3d_MyRenderer_nativeInit");
    self->priv->native_resize = (angrybirds_resize_t)LOOKUP_M("ka3d_MyRenderer_nativeResize");
    self->priv->native_input = (angrybirds_input_t)LOOKUP_M("ka3d_MyRenderer_nativeInput");
    self->priv->native_keyinput = (angrybirds_keyinput_t)LOOKUP_M("ka3d_MyRenderer_nativeKeyInput");
    self->priv->native_update = (angrybirds_update_t)LOOKUP_M("ka3d_MyRenderer_nativeUpdate");
    self->priv->native_pause = (angrybirds_pause_t)LOOKUP_M("ka3d_MyRenderer_nativePause");
    self->priv->native_resume = (angrybirds_resume_t)LOOKUP_M("ka3d_MyRenderer_nativeResume");
    self->priv->native_mixdata = (angrybirds_mixdata_t)LOOKUP_M("ka3d_AudioOutput_nativeMixData");
    self->priv->native_deinit = (angrybirds_deinit_t)LOOKUP_M("ka3d_MyRenderer_nativeDeinit");

    /* Overrides for angrybirds_jnienv_ */
    self->override_env.CallObjectMethodV = angrybirds_jnienv_CallObjectMethodV;
    self->override_env.DeleteLocalRef = angrybirds_jnienv_DeleteLocalRef;
    self->override_env.CallBooleanMethodV = angrybirds_jnienv_CallBooleanMethodV;
    self->override_env.CallStaticObjectMethodV = angrybirds_jnienv_CallStaticObjectMethodV;
    self->override_env.CallStaticVoidMethodV = angrybirds_jnienv_CallStaticVoidMethodV;
    self->override_env.NewObjectArray = angrybirds_jnienv_NewObjectArray;
    self->override_env.CallVoidMethodV = angrybirds_jnienv_CallVoidMethodV;
    self->override_env.NewObjectV = angrybirds_jnienv_NewObjectV;

    return (self->priv->native_init != NULL &&
            self->priv->native_resize != NULL &&
            self->priv->native_input != NULL &&
            self->priv->native_keyinput != NULL &&
            self->priv->native_update != NULL &&
            self->priv->native_pause != NULL &&
            self->priv->native_resume != NULL &&
            self->priv->native_mixdata != NULL &&
            self->priv->native_deinit != NULL);
}

static void
angrybirds_init(struct SupportModule *self, int width, int height, const char *home)
{
    MODULE_DEBUG_PRINTF("Module: Init(%i,%i,%s)\n",width,height,home);

    global = GLOBAL_M;

    global->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;
    global->module_hacks->glDrawArrays_rotation_hack = 1;
    global->module_hacks->gles_viewport_hack = 1;

    self->priv->myHome = strdup(home);

    int w = width, h = height;
    if(GLOBAL_M->platform->get_orientation() != ORIENTATION_LANDSCAPE) { w = height; h = width; }
    /* Java MyRenderer.onSurfaceChanged: nativeInit(w,h,filesDir) then nativeResize(w,h) */
    jboolean ok = self->priv->native_init(ENV_M, GLOBAL_M, w, h, GLOBAL_M->env->NewStringUTF(ENV_M, home));
    fprintf(stderr, "[AB] nativeInit(%d,%d,%s) -> %d\n", w, h, home, ok);
    if (ok) {
        jboolean r = self->priv->native_resize(ENV_M, GLOBAL_M, w, h);
        fprintf(stderr, "[AB] nativeResize -> %d\n", r);
    }
}

static int first_finger = -1;

static void
angrybirds_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    /* make sure first touch input is always finger == 0, seems to be required */
    if(event == ACTION_DOWN)
    {
        if(first_finger == -1)
        {
            first_finger = finger;
        }
    }

    self->priv->native_input(ENV_M, GLOBAL_M, event, x, y, (first_finger == finger) ? 0 : finger);

    if(event == ACTION_UP)
    {
        if(first_finger == finger)
        {
            first_finger = -1;
        }
    }
}

static void
angrybirds_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    self->priv->native_keyinput(ENV_M, GLOBAL_M, keycode, unicode, event == ACTION_DOWN);
}

static void
angrybirds_update(struct SupportModule *self)
{
    if (self->priv->native_update(ENV_M, GLOBAL_M) == 0)
        self->priv->want_exit = 1;
}

static void
angrybirds_deinit(struct SupportModule *self)
{
    self->priv->native_deinit(ENV_M, GLOBAL_M);
    apkenv_audio_close();
}

static void
angrybirds_pause(struct SupportModule *self)
{
    self->priv->native_pause(ENV_M, GLOBAL_M);
}

static void
angrybirds_resume(struct SupportModule *self)
{
    self->priv->native_resume(ENV_M, GLOBAL_M);
}

static int
angrybirds_requests_exit(struct SupportModule *self)
{
    return self->priv->want_exit;
}

APKENV_MODULE(angrybirds, MODULE_PRIORITY_GAME)

