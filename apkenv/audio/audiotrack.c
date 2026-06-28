/**
 * apkenv — android.media.AudioTrack streaming PCM sink. See audiotrack.h.
 */
#include "audiotrack.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Lock-free SPSC ring. head is written only by the producer (write()),
 * tail only by the consumer (SDL callback). Both are free-running byte
 * counters; the index into buf is counter & (cap-1). cap is a power of two. */
struct AudioTrack {
    unsigned char *buf;
    unsigned int   cap;          /* power of two, in bytes */
    volatile unsigned int head;  /* bytes ever written  (producer owns) */
    volatile unsigned int tail;  /* bytes ever read     (consumer owns) */
    int            rate;
    int            channels;
    volatile int   running;      /* cleared by stop() to release a blocked writer */
    unsigned long  underrun;     /* zero-filled bytes (diagnostics) */
};

/* The shared SDL device feeds exactly one track here (the FMOD pump case).
 * If multiple tracks ever coexist, mix them in the callback. */
static AudioTrack *g_active = NULL;

static unsigned int
round_pow2(unsigned int v)
{
    unsigned int p = 1;
    while (p < v && p < 0x40000000u) p <<= 1;   /* clamp: never shift past 2^30 */
    return p;
}

/* SDL pull callback — runs on the SDL audio thread. Wait-free: copies what is
 * available, zero-fills the rest (underrun), and never touches a lock. */
static void
audiotrack_sdl_callback(void *user_data, void *stream, int len)
{
    AudioTrack *t = (AudioTrack *)user_data;
    unsigned char *out = (unsigned char *)stream;

    if (t == NULL) {
        memset(out, 0, len);
        return;
    }

    unsigned int head = __atomic_load_n(&t->head, __ATOMIC_ACQUIRE);
    unsigned int tail = t->tail;
    unsigned int avail = head - tail;
    unsigned int n = ((unsigned int)len < avail) ? (unsigned int)len : avail;

    unsigned int mask = t->cap - 1;
    unsigned int ri = tail & mask;
    unsigned int first = t->cap - ri;
    if (first > n) first = n;
    memcpy(out, t->buf + ri, first);
    if (n > first)
        memcpy(out + first, t->buf, n - first);

    if ((unsigned int)len > n) {
        memset(out + n, 0, (unsigned int)len - n);
        t->underrun += (unsigned int)len - n;
    }

    __atomic_store_n(&t->tail, tail + n, __ATOMIC_RELEASE);
}

AudioTrack *
apkenv_audiotrack_create(int rate, int channels, int hint_bytes)
{
    AudioTrack *t = calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;

    /* Ring big enough to absorb the SDL device buffer plus a few engine
     * buffers of slack, so the producer's blocking write paces it cleanly. */
    unsigned int want = (hint_bytes > 0 ? (unsigned int)hint_bytes : 8192) * 4;
    if (want < 16384) want = 16384;
    t->cap = round_pow2(want);
    t->buf = malloc(t->cap);
    if (t->buf == NULL) {
        free(t);
        return NULL;
    }
    t->head = t->tail = 0;
    t->rate = rate;
    t->channels = channels;
    t->running = 1;
    t->underrun = 0;

    /* SDL device buffer in sample-frames: ~21ms keeps latency low while
     * giving the callback a comfortable chunk. Rounded to a power of two. */
    int frames = (int)round_pow2((unsigned int)(rate / 48));
    if (frames < 256) frames = 256;
    if (frames > 2048) frames = 2048;

    g_active = t;
    if (!apkenv_audio_open(rate, AudioFormat_S16SYS, channels, frames,
                           audiotrack_sdl_callback, t)) {
        fprintf(stderr, "[AudioTrack] apkenv_audio_open failed (rate=%d ch=%d)\n",
                rate, channels);
        g_active = NULL;
        free(t->buf);
        free(t);
        return NULL;
    }
    fprintf(stderr, "[AudioTrack] open rate=%d ch=%d ring=%u sdlframes=%d\n",
            rate, channels, t->cap, frames);
    return t;
}

int
apkenv_audiotrack_write(AudioTrack *t, const void *data, int bytes)
{
    if (t == NULL || bytes <= 0)
        return 0;

    const unsigned char *in = (const unsigned char *)data;
    unsigned int need = (unsigned int)bytes;
    unsigned int written = 0;
    int stall = 0;

    while (written < need && t->running) {
        unsigned int tail = __atomic_load_n(&t->tail, __ATOMIC_ACQUIRE);
        unsigned int used = t->head - tail;
        unsigned int space = t->cap - used;
        if (space == 0) {
            /* Ring full: block (back-pressure) until the callback drains.
             * ~0.5ms poll — coarse but correct; the device buffer is far
             * larger so this never starves playback. Bounded at ~0.5s so a
             * paused/closed sink (no consumer) can't wedge the writer — and
             * so the pump thread can observe a stop and exit (else join()
             * would hang). A short write under sustained back-pressure just
             * drops a buffer, which AudioTrack.write may also do. */
            if (++stall > 1000)
                break;
            usleep(500);
            continue;
        }
        stall = 0;
        unsigned int chunk = need - written;
        if (chunk > space) chunk = space;

        unsigned int mask = t->cap - 1;
        unsigned int wi = t->head & mask;
        unsigned int first = t->cap - wi;
        if (first > chunk) first = chunk;
        memcpy(t->buf + wi, in + written, first);
        if (chunk > first)
            memcpy(t->buf, in + written + first, chunk - first);

        __atomic_store_n(&t->head, t->head + chunk, __ATOMIC_RELEASE);
        written += chunk;
    }
    return (int)written;
}

void
apkenv_audiotrack_play(AudioTrack *t)
{
    if (t) apkenv_audio_play();
}

void
apkenv_audiotrack_pause(AudioTrack *t)
{
    if (t) apkenv_audio_pause();
}

void
apkenv_audiotrack_stop(AudioTrack *t)
{
    if (t == NULL)
        return;
    t->running = 0;            /* release any writer blocked in write() */
    apkenv_audio_pause();
    /* NB: do NOT touch tail here — SDL_PauseAudio only sets a flag, so the
     * callback (the sole tail-writer) may still be running; writing tail here
     * would be a two-writer race. release() calls apkenv_audio_close(), which
     * joins the audio thread, before freeing the buffer, so no stale data is
     * ever read after the device is gone. */
}

void
apkenv_audiotrack_release(AudioTrack *t)
{
    if (t == NULL)
        return;
    apkenv_audiotrack_stop(t);
    if (g_active == t) {
        apkenv_audio_close();
        g_active = NULL;
    }
    free(t->buf);
    free(t);
}

unsigned long
apkenv_audiotrack_underrun_bytes(AudioTrack *t)
{
    return t ? t->underrun : 0;
}
