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
#include <errno.h>

/* org.fmod.FMODAudioDevice fmodGetInfo() selectors (from the decompiled glue) */
#define FMOD_INFO_SAMPLERATE      0
#define FMOD_INFO_DSPBUFFERLENGTH 1
#define FMOD_INFO_DSPNUMBUFFERS   2
#define FMOD_INFO_MIXERRUNNING    3
#define FMOD_NUMCHANNELS          2   /* stereo */
#define FMOD_BYTES_PER_SAMPLE     2   /* S16 */

typedef jint (*fmod_initjni_t)(JNIEnv *, jobject) SOFTFP;
typedef jint (*fmod_getinfo_t)(JNIEnv *, jobject, jint) SOFTFP;
typedef jint (*fmod_process_t)(JNIEnv *, jobject, jobject) SOFTFP;

static fmod_initjni_t f_initjni;
static fmod_getinfo_t f_getinfo;
static fmod_process_t f_process;
static JNIEnv        *g_env;
static jobject        g_thiz;     /* FMOD ignores it (uses a global System) */
static volatile int   g_running;
static int            g_started;
static pthread_t      g_thread;

static int
pcm_read_loop(FILE *file, void *buffer, int bytes)
{
    unsigned char *dst = buffer;
    int done = 0;
    int empty_rewinds = 0;

    while (done < bytes) {
        size_t n = fread(dst + done, 1, (size_t)(bytes - done), file);
        if (n > 0) {
            done += (int)n;
            empty_rewinds = 0;
            continue;
        }
        if (ferror(file))
            return 0;
        if (++empty_rewinds > 1)
            return 0;
        clearerr(file);
        if (fseek(file, 0, SEEK_SET) != 0)
            return 0;
    }
    return 1;
}

static void
pcm_mix_s16(short *dst, const short *src, int samples, int gain_q15)
{
    int i;
    for (i = 0; i < samples; i++) {
        int mixed = (int)dst[i] + ((int)src[i] * gain_q15) / 32768;
        if (mixed > 32767) mixed = 32767;
        else if (mixed < -32768) mixed = -32768;
        dst[i] = (short)mixed;
    }
}

/* The decompiled FMODAudioDevice.run(), in C. */

/* Thread sampler (APKENV_THREAD_SAMPLE=1): every ~5 s, list every thread in
 * the process with the syscall it is blocked in (/proc/self/task/<tid>/syscall
 * gives the number and arguments) and its state. This answers, without a
 * debugger the jail forbids, "what is FMOD's thread waiting on" - a stream
 * that is primed once and never refilled is a thread that never wakes. */
#include <dirent.h>
static void
apkenv_sample_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    if (d == NULL) return;
    fprintf(stderr, "[THREADS] ---- sample ----\n");
    while ((e = readdir(d)) != NULL) {
        char path[128], buf[256], comm[64] = "?", st[16] = "?";
        FILE *f;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        if ((f = fopen(path, "r")) != NULL) { if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
        if ((f = fopen(path, "r")) != NULL) {
            if (fgets(buf, sizeof(buf), f)) { char *p = strrchr(buf, ')'); if (p && p[1]) { st[0] = p[2]; st[1] = 0; } }
            fclose(f);
        }
        snprintf(path, sizeof(path), "/proc/self/task/%s/syscall", e->d_name);
        buf[0] = 0;
        if ((f = fopen(path, "r")) != NULL) { if (fgets(buf, sizeof(buf), f)) buf[strcspn(buf, "\n")] = 0; fclose(f); }
        snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", e->d_name);
        { char wc[64] = ""; if ((f = fopen(path, "r")) != NULL) { if (fgets(wc, sizeof(wc), f)) wc[strcspn(wc, "\n")] = 0; fclose(f); }
          fprintf(stderr, "[THREADS] tid=%s state=%s comm=%s wchan=%s syscall=%s\n",
                  e->d_name, st, comm, wc, buf); }
    }
    closedir(d);
}

static void *
fmod_pump_thread(void *arg)
{
    JNIEnv     *env  = g_env;
    AudioTrack *track = NULL;
    jobject     bb = NULL;      /* fake direct ByteBuffer over chunk[] */
    void       *chunk = NULL;
    int         chunk_bytes = 0;
    int         initialised = 0;
    const int   meter = (getenv("APKENV_AUDIO_METER") != NULL);
    const char *music_path = getenv("APKENV_FMOD_MUSIC_PCM");
    FILE       *music = NULL;
    void       *music_chunk = NULL;
    int         music_gain = 16384;     /* 0.5 in Q15 */
    int         meter_peak = 0, meter_chunks = 0;
    const int   meter_every = 90;      /* ~2 s at 512-frame chunks / 24 kHz */

    (void)arg;

    if (music_path != NULL && music_path[0] != '\0') {
        const char *gain = getenv("APKENV_FMOD_MUSIC_GAIN");
        if (gain != NULL) {
            char *end = NULL;
            double value = strtod(gain, &end);
            if (end != gain && value == value) {
                if (value < 0.0) value = 0.0;
                if (value > 1.0) value = 1.0;
                music_gain = (int)(value * 32768.0);
            }
        }
        music = fopen(music_path, "rb");
        if (music != NULL) {
            long size = -1;
            if (fseek(music, 0, SEEK_END) == 0) size = ftell(music);
            if (size <= 0 || (size & 3) != 0 || fseek(music, 0, SEEK_SET) != 0) {
                fprintf(stderr, "[FMOD-MUSIC] invalid stereo S16 PCM size %ld; disabling\n",
                        size);
                fclose(music);
                music = NULL;
            } else {
                fprintf(stderr, "[FMOD-MUSIC] fallback '%s' bytes=%ld gain=%.2f\n",
                        music_path, size, (double)music_gain / 32768.0);
            }
        } else {
            fprintf(stderr, "[FMOD-MUSIC] cannot open fallback '%s': %s\n",
                    music_path, strerror(errno));
        }
    }

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
                free(music_chunk);
                music_chunk = music != NULL ? malloc(chunk_bytes) : NULL;
                if (chunk == NULL) {
                    fprintf(stderr, "[FMOD] chunk alloc failed (%d bytes)\n", chunk_bytes);
                    usleep(100000);
                    continue;
                }
                if (music != NULL && music_chunk == NULL) {
                    fprintf(stderr, "[FMOD-MUSIC] chunk alloc failed; disabling fallback\n");
                    fclose(music);
                    music = NULL;
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

                if (music != NULL && music_chunk != NULL) {
                    if (pcm_read_loop(music, music_chunk, chunk_bytes))
                        pcm_mix_s16(chunk, music_chunk, chunk_bytes / 2, music_gain);
                    else {
                        fprintf(stderr, "[FMOD-MUSIC] fallback read failed; disabling\n");
                        fclose(music);
                        music = NULL;
                    }
                }

                /* Output level meter (APKENV_AUDIO_METER=1). This is the one
                 * place that sees exactly what FMOD's mixer produced, which
                 * separates "the engine never started the music" from "the
                 * music is mixed but our output path loses it". Silence here
                 * while a track should be playing means the fault is upstream,
                 * in the engine, not in this pump. */
                if (meter) {
                    const short *sp = (const short *)chunk;
                    int n = chunk_bytes / 2, i2, peak = 0;
                    for (i2 = 0; i2 < n; i2++) {
                        int v = sp[i2] < 0 ? -sp[i2] : sp[i2];
                        if (v > peak) peak = v;
                    }
                    if (peak > meter_peak) meter_peak = peak;
                    if (++meter_chunks >= meter_every) {
                        fprintf(stderr, "[FMOD-METER] peak=%d/32767 over %d chunks%s\n",
                                meter_peak, meter_chunks,
                                meter_peak == 0 ? "  (SILENCE)" : "");
                        meter_peak = 0;
                        meter_chunks = 0;
                    }
                }

                apkenv_audiotrack_write(track, chunk, chunk_bytes); /* blocks = pacing */
                if (getenv("APKENV_THREAD_SAMPLE") != NULL) {
                    static int c; if (++c % 230 == 0) apkenv_sample_threads();
                }
            } else {
                initialised = 0;   /* mixer stopped; rebuild on next start */
            }
        }
    }

    if (track) apkenv_audiotrack_release(track);
    if (music) fclose(music);
    if (bb) free(bb);
    free(music_chunk);
    free(chunk);
    return NULL;
}

void
apkenv_fmod_pump_start(struct GlobalState *global)
{
    if (g_started)
        return;

    /* org.fmod.FMODAudioDevice.start() calls fmodInitJni() BEFORE it starts the
     * audio thread (read from FMODAudioDevice.smali). It is how FMOD's Android
     * layer caches the JNI handles it needs from native code; the mixer runs
     * without it, so its absence is invisible until something in that layer is
     * needed. Skipping a call the real host makes is exactly the kind of gap
     * that shows up later as one subsystem silently not working. */
    f_initjni = (fmod_initjni_t)global->lookup_symbol(
            "Java_org_fmod_FMODAudioDevice_fmodInitJni");
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

    if (f_initjni != NULL) {
        jint r = f_initjni(g_env, g_thiz);
        fprintf(stderr, "[FMOD] fmodInitJni -> %d\n", (int)r);
    } else {
        fprintf(stderr, "[FMOD] fmodInitJni not exported by this engine build\n");
    }

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
