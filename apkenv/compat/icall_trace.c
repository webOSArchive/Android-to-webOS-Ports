/* compat/icall_trace.c - watch a Unity engine's managed->native audio boundary
 * without touching engine code.
 *
 * Unity registers every UnityEngine.* internal call at startup with
 * mono_add_internal_call(name, fn). With APKENV_HOST_MONO that function is
 * ours (compat/hostlib.c bridges it), so apkenv can wrap it: for a curated list
 * of audio icalls whose arguments fit in r0-r3 - so a plain C trampoline
 * forwards them bit-exactly - we register a trampoline in place of Unity's
 * function. It calls through, then logs arguments and result.
 *
 * Why this boundary: libunity exports FMOD's C++ API but calls it directly,
 * never through its PLT, so FMOD itself cannot be interposed. Aralon's music is
 * requested (ZoneMusicMgr runs) yet never reaches the mixer; logging Play() with
 * the clip's length, isReadyToPlay and the source's isPlaying straight after
 * separates "Unity never starts the channel" from "the channel runs silent".
 *
 * Returns travel back as long long, which keeps r0:r1 intact for pointer, bool,
 * softfp float (in r0) and 64-bit results alike.
 *
 * Opt-in: APKENV_UNITY_ICALL_TRACE=1. Must be installed before the host-Mono
 * bridge, which leaves already-hooked names alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hooks.h"
#include "hostlib.h"

typedef void (*add_icall_t)(const char *, const void *);
typedef long long (*fn4_t)(int, int, int, int);

/* Every entry takes at most four words of arguments. */
static const char *const watch[] = {
    "UnityEngine.AudioSource::Play",               /* (this, ulong delay)          */
    "UnityEngine.AudioSource::PlayOneShot",        /* (this, clip, float volume)   */
    "UnityEngine.AudioSource::INTERNAL_CALL_Stop", /* (this)                       */
    "UnityEngine.AudioSource::get_isPlaying",      /* (this) -> bool               */
    "UnityEngine.AudioSource::set_clip",           /* (this, clip)                 */
    "UnityEngine.AudioSource::get_clip",           /* (this) -> clip               */
    "UnityEngine.AudioSource::set_volume",         /* (this, float)                */
    "UnityEngine.AudioSource::set_mute",           /* (this, bool)                 */
    "UnityEngine.AudioSource::set_loop",           /* (this, bool)                 */
    "UnityEngine.AudioClip::get_isReadyToPlay",    /* (this) -> bool               */
    "UnityEngine.AudioClip::get_length",           /* (this) -> float              */
    "UnityEngine.AudioListener::set_volume",       /* (float)                      */
    NULL
};

#define ICALL_SLOTS 16
static struct {
    const char *name;
    fn4_t real;
    unsigned long calls;
} slot[ICALL_SLOTS];
static int nslots;

/* Originals, called directly (never through a trampoline) for context. */
static fn4_t f_get_clip, f_get_isPlaying, f_clip_length, f_clip_ready;

static float
as_float(int bits)
{
    union { int i; float f; } u;
    u.i = bits;
    return u.f;
}

/* "clip=<ptr> len=<s> ready=<0/1>" - length tells music (tens of seconds and
 * up) from effects (a few seconds). */
static void
clip_desc(char *out, size_t n, int clip)
{
    float len = -1.0f;
    int ready = -1;
    if (clip != 0 && f_clip_length != NULL) len = as_float((int)f_clip_length(clip, 0, 0, 0));
    if (clip != 0 && f_clip_ready != NULL)  ready = (int)(f_clip_ready(clip, 0, 0, 0) & 1);
    snprintf(out, n, "clip=%p len=%.1fs ready=%d", (void *)clip, len, ready);
}

/* Why a music clip is "ready with length 0": Unity's clip loader
 * (libunity+0xdbe80) calls FMOD::System::createSound and, on failure, stores
 * FMOD's error string at AudioManager+156 and the FMOD_RESULT at +180, then
 * returns a NULL sound WITHOUT printing anything. The AudioManager is context
 * manager 3; libunity has a getter stub for it at +0xd2454, and its own copy of
 * FMOD_ErrorString at +0xce898. Both offsets - and the field offsets - were
 * read out of Aralon's Unity 4.0.1 libunity, so every instruction word is
 * verified before anything is called; any other build is skipped. Read-only. */
int apkenv_android_dladdr(const void *addr, void *info);

static void
fmod_last_error(const void *addr_in_libunity)
{
    struct { const char *fname; void *fbase; const char *sname; void *saddr; } di;
    static int checked = 0, ok = 0;
    static unsigned char *base;
    void *(*get_am)(void);
    const char *(*errstr)(int);
    unsigned char *am;
    int code;

    if (!checked) {
        checked = 1;
        memset(&di, 0, sizeof(di));
        if (addr_in_libunity != NULL && apkenv_android_dladdr(addr_in_libunity, &di) && di.fbase != NULL) {
            base = (unsigned char *)di.fbase;
            ok = *(unsigned *)(base + 0xd2454) == 0xe3a00003u &&   /* mov r0, #3     */
                 *(unsigned *)(base + 0xd2458) == 0xea003579u &&   /* b   dfa44      */
                 *(unsigned *)(base + 0xce898) == 0xe350005fu &&   /* cmp r0, #95    */
                 *(unsigned *)(base + 0xce89c) == 0x908ff100u;     /* addls pc, ...  */
        }
        fprintf(stderr, "[ICALL] AudioManager probe %s (libunity base %p)\n",
                ok ? "armed" : "OFF - not the build these offsets came from", (void *)base);
    }
    if (!ok)
        return;
    get_am = (void *(*)(void))(base + 0xd2454);
    errstr = (const char *(*)(int))(base + 0xce898);
    am = (unsigned char *)get_am();
    if (am == NULL) {
        fprintf(stderr, "[ICALL]   AudioManager = NULL\n");
        return;
    }
    code = *(int *)(am + 180);
    fprintf(stderr, "[ICALL]   AudioManager=%p FMOD::System=%p last createSound FMOD_RESULT=%d (%s)\n",
            (void *)am, *(void **)(am + 116), code,
            (code >= 0 && code <= 95) ? errstr(code) : "out of range");
}

static void
describe(int i, unsigned long n, int a, int b, int c, int d, long long r)
{
    const char *m = strstr(slot[i].name, "::") + 2;
    char cd[96];

    /* class first: AudioListener::set_volume and AudioSource::set_volume share
     * a method name but not a signature (static float vs this + float) */
    if (strstr(slot[i].name, "AudioListener") != NULL) {
        fprintf(stderr, "[ICALL] #%lu AudioListener.%s(%.3f)\n", n, m, as_float(a));
    } else if (strcmp(m, "Play") == 0) {
        int clip = f_get_clip ? (int)f_get_clip(a, 0, 0, 0) : 0;
        int playing = f_get_isPlaying ? (int)(f_get_isPlaying(a, 0, 0, 0) & 1) : -1;
        float len = (clip != 0 && f_clip_length != NULL) ? as_float((int)f_clip_length(clip, 0, 0, 0)) : -1.0f;
        clip_desc(cd, sizeof(cd), clip);
        fprintf(stderr, "[ICALL] #%lu AudioSource.Play(src=%p, delay=%llu) %s -> isPlaying now=%d\n",
                n, (void *)a, ((unsigned long long)(unsigned)d << 32) | (unsigned)c, cd, playing);
        /* an empty clip: ask the AudioManager what createSound said */
        if (len <= 0.0f)
            fmod_last_error((const void *)slot[i].real);
    } else if (strcmp(m, "PlayOneShot") == 0) {
        clip_desc(cd, sizeof(cd), b);
        fprintf(stderr, "[ICALL] #%lu AudioSource.PlayOneShot(src=%p, %s, vol=%.2f)\n",
                n, (void *)a, cd, as_float(c));
    } else if (strcmp(m, "set_clip") == 0) {
        clip_desc(cd, sizeof(cd), b);
        fprintf(stderr, "[ICALL] #%lu AudioSource.set_clip(src=%p, %s)\n", n, (void *)a, cd);
    } else if (strcmp(m, "set_volume") == 0) {
        fprintf(stderr, "[ICALL] #%lu AudioSource.set_volume(src=%p, %.3f)\n", n, (void *)a, as_float(b));
    } else if (strcmp(m, "set_mute") == 0 || strcmp(m, "set_loop") == 0) {
        fprintf(stderr, "[ICALL] #%lu AudioSource.%s(src=%p, %d)\n", n, m, (void *)a, b & 1);
    } else if (strcmp(m, "get_isPlaying") == 0) {
        fprintf(stderr, "[ICALL] #%lu AudioSource.get_isPlaying(src=%p) -> %d\n", n, (void *)a, (int)(r & 1));
    } else if (strcmp(m, "INTERNAL_CALL_Stop") == 0) {
        fprintf(stderr, "[ICALL] #%lu AudioSource.Stop(src=%p)\n", n, (void *)a);
    } else if (strcmp(m, "get_isReadyToPlay") == 0) {
        fprintf(stderr, "[ICALL] #%lu AudioClip.get_isReadyToPlay(clip=%p) -> %d\n", n, (void *)a, (int)(r & 1));
    } else if (strcmp(m, "get_length") == 0) {
        fprintf(stderr, "[ICALL] #%lu AudioClip.get_length(clip=%p) -> %.2f\n", n, (void *)a, as_float((int)r));
    } else if (strcmp(m, "set_volume") == 0 || strstr(slot[i].name, "AudioListener") != NULL) {
        fprintf(stderr, "[ICALL] #%lu AudioListener.set_volume(%.3f)\n", n, as_float(a));
    } else {
        fprintf(stderr, "[ICALL] #%lu %s(%x, %x, %x, %x) -> %llx\n", n, slot[i].name, a, b, c, d, r);
    }
}

static long long
tramp(int i, int a, int b, int c, int d)
{
    long long r = slot[i].real(a, b, c, d);
    unsigned long n = ++slot[i].calls;
    /* bounded: get_isPlaying and friends can run every frame */
    if (n <= 40 || (n % 1000) == 0)
        describe(i, n, a, b, c, d, r);
    return r;
}

#define T(k) static long long tramp_##k(int a, int b, int c, int d) { return tramp(k, a, b, c, d); }
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9) T(10) T(11) T(12) T(13) T(14) T(15)
static const fn4_t tramps[ICALL_SLOTS] = {
    tramp_0, tramp_1, tramp_2, tramp_3, tramp_4, tramp_5, tramp_6, tramp_7,
    tramp_8, tramp_9, tramp_10, tramp_11, tramp_12, tramp_13, tramp_14, tramp_15
};

static add_icall_t real_add;

static void
my_mono_add_internal_call(const char *name, const void *fn)
{
    int i;

    if (real_add == NULL)
        real_add = (add_icall_t)apkenv_hostlib_dlsym("libmono.so", "mono_add_internal_call");
    if (real_add == NULL) {
        fprintf(stderr, "[ICALL] cannot resolve the host mono_add_internal_call - %s dropped\n", name);
        return;
    }
    for (i = 0; name != NULL && watch[i] != NULL; i++) {
        if (strcmp(name, watch[i]) != 0)
            continue;
        if (strstr(name, "::get_clip"))          f_get_clip = (fn4_t)fn;
        if (strstr(name, "::get_isPlaying"))     f_get_isPlaying = (fn4_t)fn;
        if (strstr(name, "Clip::get_length"))    f_clip_length = (fn4_t)fn;
        if (strstr(name, "::get_isReadyToPlay")) f_clip_ready = (fn4_t)fn;
        if (nslots < ICALL_SLOTS) {
            int s = nslots++;
            slot[s].name = watch[i];
            slot[s].real = (fn4_t)fn;
            fprintf(stderr, "[ICALL] tracing %s (real %p)\n", name, fn);
            real_add(name, (const void *)tramps[s]);
            return;
        }
        break;
    }
    real_add(name, fn);
}

void
apkenv_icall_trace_install(void)
{
    static const struct _hook h[] = {
        { "mono_add_internal_call", (void *)my_mono_add_internal_call },
    };
    const char *e = getenv("APKENV_UNITY_ICALL_TRACE");

    if (e == NULL || e[0] != '1')
        return;
    if (register_hooks(h, 1) != 0) {
        fprintf(stderr, "[ICALL] register_hooks failed - tracer off\n");
        return;
    }
    fprintf(stderr, "[ICALL] audio icall tracer armed (wraps mono_add_internal_call)\n");
}
