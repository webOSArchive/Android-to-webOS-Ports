/**
 * openslestest — host unit test for compat/opensles.c (the libOpenSLES shim).
 *
 * Drives the shim the way Tiny Death Star's libfmodex does, calling every
 * method through the RAW vtable slot offsets read out of libfmodex's
 * disassembly (fmod_output_opensl.cpp), not through a header — so a slot
 * order mistake in the shim fails here instead of jumping somewhere on device.
 *
 *   gcc -O1 -Wall -DOPENSLES_UNIT_TEST -I. tools/openslestest.c compat/opensles.c \
 *       -o openslestest -lpthread && ./openslestest
 */
#include "compat/opensles.h"
#include "audio/audiotrack.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── stub AudioTrack: counts bytes, blocks ~1 ms per write like a sink ─────── */
struct AudioTrack { int rate, ch; };
static unsigned long stub_bytes, stub_writes;
static int stub_playing, stub_released;
AudioTrack *apkenv_audiotrack_create(int rate, int channels, int hint)
{
    AudioTrack *t = calloc(1, sizeof(*t));
    t->rate = rate; t->ch = channels;
    printf("stub: audiotrack_create(%d, %d, hint %d)\n", rate, channels, hint);
    return t;
}
int apkenv_audiotrack_write(AudioTrack *t, const void *d, int n)
{
    const short *s = d;
    if (n >= 2 && s[0] != (short)(stub_writes & 0x7fff))
        printf("stub: WRONG buffer order: sample %d, expected %lu\n", s[0], stub_writes);
    stub_bytes += (unsigned long)n; stub_writes++; usleep(1000); return n;
}
void apkenv_audiotrack_play(AudioTrack *t) { stub_playing = 1; }
void apkenv_audiotrack_pause(AudioTrack *t) { stub_playing = 0; }
void apkenv_audiotrack_stop(AudioTrack *t) { stub_playing = 0; }
void apkenv_audiotrack_release(AudioTrack *t) { stub_released = 1; free(t); }
unsigned long apkenv_audiotrack_underrun_bytes(AudioTrack *t) { return 0; }

/* ── raw-slot calling, FMOD style ─────────────────────────────────────────── */
typedef void *itf_t;                         /* pointer to pointer to vtable */
#define SLOT(itf, off) (((void **)*(void **)(itf))[(off) / 4])
typedef unsigned int u32;

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define NUMBUF 4
#define BUFBYTES 2048
static short bufs[NUMBUF][BUFBYTES / 2];
static int next_fill = 0;
static int ring_idx = 0;
static volatile int callbacks = 0;
static itf_t g_bq;

static void enqueue_next(itf_t bq)
{
    u32 (*Enqueue)(itf_t, const void *, u32) = SLOT(bq, 0);
    short *b = bufs[ring_idx];
    b[0] = (short)(next_fill++ & 0x7fff);   /* sequence number in sample 0 */
    u32 r = Enqueue(bq, b, BUFBYTES);
    CHECK(r == 0, "Enqueue #%d -> %u", next_fill - 1, r);
    ring_idx = (ring_idx + 1) % NUMBUF;
}

static void bq_callback(itf_t caller, void *ctx)
{
    CHECK(caller == g_bq, "callback caller %p != bq %p", caller, g_bq);
    CHECK(ctx == (void *)0x1234, "callback ctx %p", ctx);
    callbacks++;
    enqueue_next(caller);                    /* refill from inside the callback */
}

int main(void)
{
    itf_t engine_obj = NULL, engine = NULL, mix = NULL, player = NULL, cfg = NULL,
          play = NULL, bq = NULL, rec = NULL, vol = NULL;
    u32 r;

    /* dlsym before enable is what hooks.c gates on; the table itself answers */
    apkenv_opensles_enable();
    CHECK(apkenv_opensles_enabled(), "not enabled");

    u32 (*slCreateEngine)(itf_t *, u32, void *, u32, void *, void *) =
        apkenv_opensles_dlsym("slCreateEngine");
    void **iid_engine = apkenv_opensles_dlsym("SL_IID_ENGINE");
    void **iid_bq     = apkenv_opensles_dlsym("SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    void **iid_cfg    = apkenv_opensles_dlsym("SL_IID_ANDROIDCONFIGURATION");
    void **iid_play   = apkenv_opensles_dlsym("SL_IID_PLAY");
    void **iid_rec    = apkenv_opensles_dlsym("SL_IID_RECORD");
    void **iid_vol    = apkenv_opensles_dlsym("SL_IID_VOLUME");
    CHECK(slCreateEngine && iid_engine && iid_bq && iid_cfg && iid_play && iid_rec,
          "missing export");
    CHECK(apkenv_opensles_dlsym("SL_IID_BOGUS") == NULL, "bogus symbol resolved");

    /* slCreateEngine(&obj, 0, NULL, 0, NULL, NULL) -> Realize(+0) -> GetInterface(+12) */
    r = slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL);
    CHECK(r == 0 && engine_obj, "slCreateEngine %u", r);
    r = ((u32 (*)(itf_t, u32))SLOT(engine_obj, 0))(engine_obj, 0);
    CHECK(r == 0, "engine Realize %u", r);
    r = ((u32 (*)(itf_t, void *, itf_t *))SLOT(engine_obj, 12))(engine_obj, *iid_engine, &engine);
    CHECK(r == 0 && engine, "GetInterface ENGINE %u", r);

    /* engine +28 CreateOutputMix(engine, &mix, 0, NULL, NULL); Realize */
    r = ((u32 (*)(itf_t, itf_t *, u32, void *, void *))SLOT(engine, 28))(engine, &mix, 0, NULL, NULL);
    CHECK(r == 0 && mix, "CreateOutputMix %u", r);
    r = ((u32 (*)(itf_t, u32))SLOT(mix, 0))(mix, 0);
    CHECK(r == 0, "mix Realize %u", r);

    /* engine +8 CreateAudioPlayer: layout exactly as FMOD builds it on its stack */
    u32 loc_bq[2]  = { 0x800007BD, NUMBUF };
    u32 pcm[7]     = { 2, 2, 44100000, 16, 16, 3, 2 };
    void *src[2]   = { loc_bq, pcm };
    void *loc_mix[2] = { (void *)(unsigned long)4, mix };
    void *snk[2]   = { loc_mix, NULL };
    void *ids[2]   = { *iid_bq, *iid_cfg };
    u32 req[2]     = { 1, 1 };
    r = ((u32 (*)(itf_t, itf_t *, void *, void *, u32, void *, void *))SLOT(engine, 8))
            (engine, &player, src, snk, 2, ids, req);
    CHECK(r == 0 && player, "CreateAudioPlayer %u", r);

    /* engine +12 CreateAudioRecorder must fail cleanly */
    r = ((u32 (*)(itf_t, itf_t *, void *, void *, u32, void *, void *))SLOT(engine, 12))
            (engine, &rec, NULL, NULL, 0, NULL, NULL);
    CHECK(r == 12 && rec == NULL, "CreateAudioRecorder -> %u (want 12)", r);

    /* configuration BEFORE Realize: +12 GetInterface, cfg +0 SetConfiguration */
    r = ((u32 (*)(itf_t, void *, itf_t *))SLOT(player, 12))(player, *iid_cfg, &cfg);
    CHECK(r == 0 && cfg, "GetInterface ANDROIDCONFIGURATION pre-realize %u", r);
    u32 stream = 3;
    r = ((u32 (*)(itf_t, const char *, void *, u32))SLOT(cfg, 0))
            (cfg, "androidPlaybackStreamType", &stream, 4);
    CHECK(r == 0, "SetConfiguration %u", r);

    r = ((u32 (*)(itf_t, u32))SLOT(player, 0))(player, 0);
    CHECK(r == 0, "player Realize %u", r);
    r = ((u32 (*)(itf_t, void *, itf_t *))SLOT(player, 12))(player, *iid_play, &play);
    CHECK(r == 0 && play, "GetInterface PLAY %u", r);
    r = ((u32 (*)(itf_t, void *, itf_t *))SLOT(player, 12))(player, *iid_bq, &bq);
    CHECK(r == 0 && bq, "GetInterface BUFFERQUEUE %u", r);
    r = ((u32 (*)(itf_t, void *, itf_t *))SLOT(player, 12))(player, *iid_vol, &vol);
    CHECK(r == 0 && vol, "GetInterface VOLUME %u", r);
    r = ((u32 (*)(itf_t, void *, itf_t *))SLOT(player, 12))(player, *iid_rec, &rec);
    CHECK(r == 12 && rec == NULL, "GetInterface RECORD on player -> %u", r);
    g_bq = bq;

    /* bq +12 RegisterCallback(bq, cb, ctx) */
    r = ((u32 (*)(itf_t, void *, void *))SLOT(bq, 12))(bq, bq_callback, (void *)0x1234);
    CHECK(r == 0, "RegisterCallback %u", r);

    /* FMOD primes the whole queue, so the queue must hold numBuffers and
     * reject one more. */
    int i;
    for (i = 0; i < NUMBUF; i++) enqueue_next(bq);
    {
        u32 st[2];
        r = ((u32 (*)(itf_t, u32 *))SLOT(bq, 8))(bq, st);
        CHECK(r == 0 && st[0] == NUMBUF, "GetState count %u (want %d)", st[0], NUMBUF);
        r = ((u32 (*)(itf_t, const void *, u32))SLOT(bq, 0))(bq, bufs[0], BUFBYTES);
        CHECK(r == 7, "Enqueue on full queue -> %u (want 7 BUFFER_INSUFFICIENT)", r);
    }

    /* play +0 SetPlayState(PLAYING); let callbacks refill ~200 times */
    r = ((u32 (*)(itf_t, u32))SLOT(play, 0))(play, 3);
    CHECK(r == 0 && stub_playing, "SetPlayState PLAYING %u", r);
    for (i = 0; i < 2000 && callbacks < 200; i++) usleep(1000);
    CHECK(callbacks >= 200, "only %d callbacks", callbacks);

    /* pause: no progress */
    r = ((u32 (*)(itf_t, u32))SLOT(play, 0))(play, 2);
    usleep(20000);
    {
        unsigned long w = stub_writes;
        usleep(50000);
        CHECK(stub_writes == w, "writes continued while paused (%lu -> %lu)", w, stub_writes);
    }
    {
        u32 ps = 0;
        ((u32 (*)(itf_t, u32 *))SLOT(play, 4))(play, &ps);
        CHECK(ps == 2, "GetPlayState %u", ps);
    }
    /* resume, then stop + Clear + Destroy (join) */
    ((u32 (*)(itf_t, u32))SLOT(play, 0))(play, 3);
    usleep(30000);
    ((u32 (*)(itf_t, u32))SLOT(play, 0))(play, 1);
    ((u32 (*)(itf_t))SLOT(bq, 4))(bq);
    ((void (*)(itf_t))SLOT(player, 24))(player);     /* Destroy */
    CHECK(stub_released, "AudioTrack not released on Destroy");
    ((void (*)(itf_t))SLOT(mix, 24))(mix);
    ((void (*)(itf_t))SLOT(engine_obj, 24))(engine_obj);

    printf("writes %lu, bytes %lu, callbacks %d, enqueued %d\n",
           stub_writes, stub_bytes, callbacks, next_fill);
    CHECK(stub_bytes == stub_writes * BUFBYTES, "byte count mismatch");
    printf(fails ? "RESULT: %d FAILURES\n" : "RESULT: PASS\n", fails);
    return fails != 0;
}
