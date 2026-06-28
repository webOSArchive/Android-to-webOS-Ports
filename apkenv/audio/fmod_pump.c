/**
 * apkenv — FMOD Ex AudioTrack device pump. See fmod_pump.h.
 */
#include "fmod_pump.h"
#include "audiotrack.h"

#include "../apkenv.h"   /* JNIEnv, struct GlobalState, SOFTFP, lookup_symbol */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* org.fmod.FMODAudioDevice fmodGetInfo() selectors (from the decompiled glue) */
#define FMOD_INFO_SAMPLERATE      0
#define FMOD_INFO_DSPBUFFERLENGTH 1
#define FMOD_INFO_DSPNUMBUFFERS   2
#define FMOD_INFO_MIXERRUNNING    3
#define FMOD_NUMCHANNELS          2   /* stereo */
#define FMOD_BYTES_PER_SAMPLE     2   /* S16 */

typedef jint (*fmod_getinfo_t)(JNIEnv *, jobject, jint) SOFTFP;
typedef jint (*fmod_process_t)(JNIEnv *, jobject, jobject) SOFTFP;

static fmod_getinfo_t f_getinfo;
static fmod_process_t f_process;
static JNIEnv        *g_env;
static jobject        g_thiz;     /* FMOD ignores it (uses a global System) */
static volatile int   g_running;
static int            g_started;
static pthread_t      g_thread;

/* The decompiled FMODAudioDevice.run(), in C. */
static void *
fmod_pump_thread(void *arg)
{
    JNIEnv     *env  = g_env;
    AudioTrack *track = NULL;
    jobject     bb = NULL;      /* fake direct ByteBuffer over chunk[] */
    void       *chunk = NULL;
    int         chunk_bytes = 0;
    int         initialised = 0;

    (void)arg;

    while (g_running) {
        if (!initialised) {
            jint rate = f_getinfo(env, g_thiz, FMOD_INFO_SAMPLERATE);
            if (rate > 0) {
                if (track) {
                    apkenv_audiotrack_release(track);
                    track = NULL;
                }
                jint dsp_len = f_getinfo(env, g_thiz, FMOD_INFO_DSPBUFFERLENGTH);
                jint dsp_num = f_getinfo(env, g_thiz, FMOD_INFO_DSPNUMBUFFERS);
                if (dsp_len <= 0) {
                    /* rate>0 doesn't guarantee a valid DSP buffer length yet;
                     * avoid a 0-byte chunk (busy-spin) or bogus alloc. */
                    usleep(1000);
                    continue;
                }

                /* one DSP buffer of interleaved stereo S16 = fmodProcess chunk */
                chunk_bytes = dsp_len * FMOD_BYTES_PER_SAMPLE * FMOD_NUMCHANNELS;
                int track_bytes = chunk_bytes * (dsp_num > 0 ? dsp_num : 1);

                free(chunk);
                chunk = calloc(1, chunk_bytes);
                if (chunk == NULL) {
                    fprintf(stderr, "[FMOD] chunk alloc failed (%d bytes)\n", chunk_bytes);
                    usleep(100000);
                    continue;
                }
                if (bb) free(bb);   /* fake DirectByteBuffer; free the previous one */
                bb = (*env)->NewDirectByteBuffer(env, chunk, chunk_bytes);

                track = apkenv_audiotrack_create(rate, FMOD_NUMCHANNELS, track_bytes);
                if (track == NULL) {
                    fprintf(stderr, "[FMOD] AudioTrack create failed; pump idle\n");
                    usleep(100000);
                    continue;
                }
                apkenv_audiotrack_play(track);
                initialised = 1;
                fprintf(stderr, "[FMOD] init rate=%d dspLen=%d dspNum=%d "
                        "chunk=%d track=%d\n", rate, dsp_len, dsp_num,
                        chunk_bytes, track_bytes);
            } else {
                usleep(1000);   /* FMOD not initialised yet; poll (Thread.sleep(1)) */
            }
        } else {
            if (f_getinfo(env, g_thiz, FMOD_INFO_MIXERRUNNING) == 1) {
                f_process(env, g_thiz, bb);                 /* fill chunk */
                apkenv_audiotrack_write(track, chunk, chunk_bytes); /* blocks = pacing */
            } else {
                initialised = 0;   /* mixer stopped; rebuild on next start */
            }
        }
    }

    if (track) apkenv_audiotrack_release(track);
    if (bb) free(bb);
    free(chunk);
    return NULL;
}

void
apkenv_fmod_pump_start(struct GlobalState *global)
{
    if (g_started)
        return;

    f_getinfo = (fmod_getinfo_t)global->lookup_symbol(
            "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
    f_process = (fmod_process_t)global->lookup_symbol(
            "Java_org_fmod_FMODAudioDevice_fmodProcess");
    if (f_getinfo == NULL || f_process == NULL) {
        fprintf(stderr, "[FMOD] no FMODAudioDevice natives (getinfo=%p process=%p); "
                "no audio pump\n", (void *)f_getinfo, (void *)f_process);
        return;
    }

    g_env  = ENV(global);
    g_thiz = (jobject)global;   /* unused by FMOD, just non-NULL */
    g_running = 1;
    if (pthread_create(&g_thread, NULL, fmod_pump_thread, NULL) != 0) {
        fprintf(stderr, "[FMOD] pthread_create failed; no audio pump\n");
        g_running = 0;
        return;
    }
    g_started = 1;
    fprintf(stderr, "[FMOD] audio pump started\n");
}

void
apkenv_fmod_pump_stop(void)
{
    if (!g_started)
        return;
    g_running = 0;
    pthread_join(g_thread, NULL);
    g_started = 0;
    fprintf(stderr, "[FMOD] audio pump stopped\n");
}
