/**
 * apkenv — Halfbrick **Mortar** engine support module.
 *
 * Covers the Mortar 1.8.x host generation (Fruit Ninja 1.8.8,
 * com.halfbrick.fruitninjafree). This is NOT the generation that upstream
 * modules/fruitninja.c targets: 1.8.x split init into SystemInit + GameInit,
 * grew a third path on InitFileManager, and dropped the SoundManager
 * (SFXPlayInternal / SongPlay) Java sound path entirely.
 *
 * The contract below was derived statically from classes.dex and the engine's
 * string table before any device run; see plan/FRUITNINJA.md for the full
 * table, the evidence for each entry, and the test protocol.
 *
 * Three things decide whether this game works at all:
 *
 *  1. Boot order. GLSurfaceView gives GameManager mWidth/mHeight in
 *     onSurfaceChanged, then frame 1 runs SystemInit (which itself does
 *     InitFileManager + sound init) and, with no splash, GameInit. Every frame
 *     after that is Render(): GL state, key events, step(), quit checks.
 *
 *  2. Audio. NativeGameLib.SupportsOpenSL() is true on every device but
 *     Lenovo, so Fruit Ninja normally uses OpenSL ES. webOS has none, so we
 *     answer 0 and the engine falls back to pushing its own mix at Java's
 *     MortarAudioMixerOut — an AudioTrack(44100, STEREO, PCM_16BIT,
 *     MODE_STREAM). That is exactly audio/audiotrack.c.
 *
 *  3. Touch coordinates are NORMALIZED 0..1 — MultiTouchInputHandler divides
 *     by the display size before calling native_touchEvent. Pixels here look
 *     precisely like "touch is dead" (see PORTING-PLAYBOOK.md §4).
 */

#include "common.h"
#include "../audio/audiotrack.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── engine entry points (nm -D libmortargame.so | grep Java_) ─────────────── */
typedef void     (*mortar_systeminit_t)(JNIEnv *, jobject, jint, jint, jstring) SOFTFP;
typedef void     (*mortar_gameinit_t)(JNIEnv *, jobject) SOFTFP;
typedef void     (*mortar_initfilemanager_t)(JNIEnv *, jobject, jstring, jstring, jstring, jboolean) SOFTFP;
typedef void     (*mortar_initjavasound_t)(JNIEnv *, jobject) SOFTFP;
typedef jboolean (*mortar_initopenslsound_t)(JNIEnv *, jobject, jobject) SOFTFP;
typedef jboolean (*mortar_step_t)(JNIEnv *, jobject) SOFTFP;
typedef void     (*mortar_touchevent_t)(JNIEnv *, jobject, jint, jlong, jint, jfloat, jfloat, jfloat, jfloat) SOFTFP;
typedef void     (*mortar_keyevent_t)(JNIEnv *, jobject, jint, jboolean, jboolean) SOFTFP;
typedef void     (*mortar_void_t)(JNIEnv *, jobject) SOFTFP;
typedef jboolean (*mortar_bool_t)(JNIEnv *, jobject) SOFTFP;
typedef void     (*mortar_setapplicensed_t)(JNIEnv *, jobject, jboolean) SOFTFP;

struct SupportModulePriv {
    jni_onload_t             JNI_OnLoad;
    mortar_systeminit_t      native_SystemInit;
    mortar_gameinit_t        native_GameInit;
    mortar_initfilemanager_t native_InitFileManager;
    mortar_initjavasound_t   native_InitJavaSoundManager;
    mortar_initopenslsound_t native_InitOpenSLSoundManager;
    mortar_step_t            native_step;
    mortar_touchevent_t      native_touchEvent;
    mortar_keyevent_t        native_keyEvent;
    mortar_void_t            native_onPause;
    mortar_void_t            native_onResume;
    mortar_void_t            native_onFocusLost;
    mortar_void_t            native_onFocusRetrieved;
    mortar_void_t            native_saveOnExit;
    mortar_bool_t            native_gameRequestedQuit;
    mortar_bool_t            native_gameRequestedRestart;
    mortar_setapplicensed_t  native_SetAppLicensed;

    char  home[PATH_MAX];
    int   screen_w, screen_h;
    int   game_initialized;
    int   want_exit;
};
static struct SupportModulePriv mortar_priv;
static struct GlobalState *global;

/* jni.h forward-declares struct _jfieldID opaquely; jnienv.h aliases it to
 * _jmethodID, which is what apkenv actually hands back. */
#define FIELD_NAME(f) (((struct _jmethodID *)(f))->name)

#define method_is(m) (0 == strcmp(method->name, #m))
#define sig_is(s)    (method->sig && 0 == strcmp(method->sig, (s)))

/* ── engine->host call-out tracer (PORTING-PLAYBOOK.md §3) ──────────────────
 * Every Java method the engine calls that this module does not implement is a
 * potential contract gap: print each distinct one once, then at 100/10k/1M. */
#define MORTAR_TRACE_MAX 96
static struct { char *name; unsigned long n; } mortar_trace[MORTAR_TRACE_MAX];
static int mortar_trace_n = 0;
static unsigned long mortar_frames = 0;

static void
mortar_trace_unhandled(const char *kind, jmethodID method)
{
    int i;
    for (i = 0; i < mortar_trace_n; i++)
        if (strcmp(mortar_trace[i].name, method->name) == 0) {
            unsigned long n = ++mortar_trace[i].n;
            if (n == 100 || n == 10000 || n == 1000000)
                fprintf(stderr, "[MORTAR-JNI] %s %s called %lu times (frame=%lu)\n",
                        kind, method->name, n, mortar_frames);
            return;
        }
    fprintf(stderr, "[MORTAR-JNI] UNHANDLED %s %s%s (frame=%lu)\n",
            kind, method->name, method->sig ? method->sig : "", mortar_frames);
    if (mortar_trace_n < MORTAR_TRACE_MAX) {
        mortar_trace[mortar_trace_n].name = strdup(method->name);
        mortar_trace[mortar_trace_n].n = 1;
        mortar_trace_n++;
    }
}

/* ── AES-128/192/256-CBC, NoPadding ────────────────────────────────────────
 * HBSupport.Encrypt/Decrypt are javax.crypto "AES/CBC/NoPadding" with the key
 * and IV that HBSupport.Init(key, iv) was handed. Returning the input (or
 * NULL) would break any round-trip the engine does not own both ends of, so
 * reproduce the cipher. Compact FIPS-197 implementation; no dependencies. */
/* AES-128/192/256 core, FIPS-197. State is column-major: s[c*4+r]. */
static const unsigned char aes_sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static unsigned char aes_rsbox[256];
static const unsigned char aes_rcon[11] =
    { 0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

struct aes_ctx { unsigned char rk[240]; int nr; };

static unsigned char aes_xtime(unsigned char x)
{ return (unsigned char)((x << 1) ^ ((x >> 7) * 0x1b)); }

static unsigned char aes_mul(unsigned char x, unsigned char y)
{
    unsigned char r = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (y & 1) r ^= x;
        x = aes_xtime(x);
        y >>= 1;
    }
    return r;
}

static void aes_init(struct aes_ctx *c, const unsigned char *key, int keylen)
{
    int nk = keylen / 4, i, j;
    static int rsbox_ready = 0;
    if (!rsbox_ready) {
        for (i = 0; i < 256; i++) aes_rsbox[aes_sbox[i]] = (unsigned char)i;
        rsbox_ready = 1;
    }
    c->nr = nk + 6;
    memcpy(c->rk, key, (size_t)keylen);
    for (i = nk; i < 4 * (c->nr + 1); i++) {
        unsigned char t[4];
        for (j = 0; j < 4; j++) t[j] = c->rk[(i - 1) * 4 + j];
        if (i % nk == 0) {
            unsigned char tmp = t[0];
            t[0] = (unsigned char)(aes_sbox[t[1]] ^ aes_rcon[i / nk]);
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        } else if (nk > 6 && i % nk == 4) {
            for (j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
        }
        for (j = 0; j < 4; j++) c->rk[i * 4 + j] = c->rk[(i - nk) * 4 + j] ^ t[j];
    }
}

static void aes_add_rk(unsigned char *s, const unsigned char *rk)
{ int i; for (i = 0; i < 16; i++) s[i] ^= rk[i]; }

/* s[c*4+r]: ShiftRows rotates row r left by r columns. */
static void aes_shift_rows(unsigned char *s)
{
    unsigned char t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void aes_inv_shift_rows(unsigned char *s)
{
    unsigned char t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static void aes_encrypt_block(const struct aes_ctx *c, unsigned char *s)
{
    int round, i;
    aes_add_rk(s, c->rk);
    for (round = 1; round <= c->nr; round++) {
        for (i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
        aes_shift_rows(s);
        if (round != c->nr) {
            for (i = 0; i < 16; i += 4) {
                unsigned char a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
                unsigned char x = (unsigned char)(a0 ^ a1 ^ a2 ^ a3);
                s[i]   = (unsigned char)(a0 ^ x ^ aes_xtime((unsigned char)(a0 ^ a1)));
                s[i+1] = (unsigned char)(a1 ^ x ^ aes_xtime((unsigned char)(a1 ^ a2)));
                s[i+2] = (unsigned char)(a2 ^ x ^ aes_xtime((unsigned char)(a2 ^ a3)));
                s[i+3] = (unsigned char)(a3 ^ x ^ aes_xtime((unsigned char)(a3 ^ a0)));
            }
        }
        aes_add_rk(s, c->rk + round * 16);
    }
}

static void aes_decrypt_block(const struct aes_ctx *c, unsigned char *s)
{
    int round, i;
    aes_add_rk(s, c->rk + c->nr * 16);
    for (round = c->nr - 1; round >= 0; round--) {
        aes_inv_shift_rows(s);
        for (i = 0; i < 16; i++) s[i] = aes_rsbox[s[i]];
        aes_add_rk(s, c->rk + round * 16);
        if (round != 0) {
            for (i = 0; i < 16; i += 4) {
                unsigned char a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
                s[i]   = (unsigned char)(aes_mul(a0,14)^aes_mul(a1,11)^aes_mul(a2,13)^aes_mul(a3, 9));
                s[i+1] = (unsigned char)(aes_mul(a0, 9)^aes_mul(a1,14)^aes_mul(a2,11)^aes_mul(a3,13));
                s[i+2] = (unsigned char)(aes_mul(a0,13)^aes_mul(a1, 9)^aes_mul(a2,14)^aes_mul(a3,11));
                s[i+3] = (unsigned char)(aes_mul(a0,11)^aes_mul(a1,13)^aes_mul(a2, 9)^aes_mul(a3,14));
            }
        }
    }
}

/* key/iv as given to HBSupport.Init(String,String) */
static unsigned char hb_key[32], hb_iv[16];
static int hb_keylen = 0, hb_ivlen = 0;

static struct dummy_array *
hb_crypt(const struct dummy_array *in, int encrypt)
{
    size_t nbytes, i;
    unsigned char chain[16];
    struct aes_ctx ctx;
    struct dummy_array *out;
    unsigned char *o;
    const unsigned char *p;

    if (in == NULL || in->data == NULL) return NULL;
    nbytes = (size_t)in->length * (size_t)in->element_size;
    if (nbytes == 0) return NULL;

    out = malloc(sizeof(*out));
    if (out == NULL) return NULL;
    /* NoPadding: whole blocks only; a ragged tail is copied through. */
    out->element_size = 1;
    out->length = (long)nbytes;
    out->data = malloc(nbytes);
    if (out->data == NULL) { free(out); return NULL; }
    memcpy(out->data, in->data, nbytes);

    if (hb_keylen != 16 && hb_keylen != 24 && hb_keylen != 32) {
        static int warned = 0;
        if (!warned++)
            fprintf(stderr, "[MORTAR-CRYPT] %s with no key from HBSupport.Init "
                    "(keylen=%d) — passing bytes through\n",
                    encrypt ? "Encrypt" : "Decrypt", hb_keylen);
        return out;
    }

    aes_init(&ctx, hb_key, hb_keylen);
    memset(chain, 0, sizeof(chain));
    memcpy(chain, hb_iv, hb_ivlen < 16 ? (size_t)hb_ivlen : 16);

    o = out->data;
    p = in->data;
    for (i = 0; i + 16 <= nbytes; i += 16) {
        if (encrypt) {
            int k;
            for (k = 0; k < 16; k++) o[i + k] = (unsigned char)(p[i + k] ^ chain[k]);
            aes_encrypt_block(&ctx, o + i);
            memcpy(chain, o + i, 16);
        } else {
            unsigned char prev[16];
            int k;
            memcpy(prev, p + i, 16);
            aes_decrypt_block(&ctx, o + i);
            for (k = 0; k < 16; k++) o[i + k] ^= chain[k];
            memcpy(chain, prev, 16);
        }
    }
    return out;
}

/* ── the stores Android gave the game and we now owe it ─────────────────────
 * SharedPreferences("MortarGameActivity") and KeyStore.dat. Flat "key=value"
 * text in the per-apk data dir; small enough to rewrite on every set. */
struct kv { char *k, *v; struct kv *next; };

struct store {
    const char *file;          /* basename in the data dir */
    struct kv *head;
    int loaded;
};
static struct store prefs_store   = { "prefs.txt",    NULL, 0 };
static struct store keystore_store = { "KeyStore.dat", NULL, 0 };

static void
store_path(const struct store *s, char *out, size_t n)
{
    snprintf(out, n, "%s%s", mortar_priv.home, s->file);
}

static void
store_load(struct store *s)
{
    char path[PATH_MAX], line[1024];
    FILE *f;
    if (s->loaded) return;
    s->loaded = 1;
    store_path(s, path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n"), *eq;
        if (nl) *nl = '\0';
        eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        struct kv *e = malloc(sizeof(*e));
        e->k = strdup(line);
        e->v = strdup(eq + 1);
        e->next = s->head;
        s->head = e;
    }
    fclose(f);
}

static void
store_save(struct store *s)
{
    char path[PATH_MAX];
    struct kv *e;
    FILE *f;
    store_path(s, path, sizeof(path));
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "[MORTAR-STORE] cannot write %s\n", path);
        return;
    }
    for (e = s->head; e; e = e->next)
        fprintf(f, "%s=%s\n", e->k, e->v);
    fclose(f);
}

static const char *
store_get(struct store *s, const char *k)
{
    struct kv *e;
    store_load(s);
    for (e = s->head; e; e = e->next)
        if (strcmp(e->k, k) == 0) return e->v;
    return NULL;
}

static void
store_set(struct store *s, const char *k, const char *v)
{
    struct kv *e;
    store_load(s);
    for (e = s->head; e; e = e->next)
        if (strcmp(e->k, k) == 0) {
            free(e->v);
            e->v = strdup(v);
            store_save(s);
            return;
        }
    e = malloc(sizeof(*e));
    e->k = strdup(k);
    e->v = strdup(v);
    e->next = s->head;
    s->head = e;
    store_save(s);
}

/* ── audio: MortarAudioMixerOut == AudioTrack(44100, STEREO, S16, STREAM) ─── */
#define MORTAR_AUDIO_RATE     44100
#define MORTAR_AUDIO_CHANNELS 2

/* Layout-compatible with struct dummy_jclass (name first), so GetObjectClass
 * can name one of our objects instead of handing back a sentinel. */
struct mortar_object { char *name; };
static struct mortar_object audio_mixer_obj = { (char *)"com/halfbrick/mortar/MortarAudioMixerOut" };
static struct dummy_jclass dialog_class = { (char *)"com/halfbrick/mortar/MortarDialog" };

static AudioTrack *audio_track = NULL;
static unsigned long audio_writes = 0;
static unsigned long audio_bytes = 0;

/* Opened on the main thread at module init: WriteData arrives on the engine's
 * own "Audio Thread", and SDL_OpenAudio is not something to do from there. */
static void
mortar_audio_open(void)
{
    if (audio_track != NULL)
        return;
    /* getMinBufferSize() on Android 2.3 for 44100/stereo/S16 is a few KB; the
     * ring is sized off this hint (audio/audiotrack.c). */
    audio_track = apkenv_audiotrack_create(MORTAR_AUDIO_RATE, MORTAR_AUDIO_CHANNELS, 8192);
    if (audio_track == NULL) {
        fprintf(stderr, "[MORTAR-AUDIO] AudioTrack create FAILED — the game will be silent\n");
        return;
    }
    fprintf(stderr, "[MORTAR-AUDIO] AudioTrack %d/%d open\n",
            MORTAR_AUDIO_RATE, MORTAR_AUDIO_CHANNELS);
    apkenv_audiotrack_play(audio_track);
}

static void
mortar_audio_write(const void *data, int bytes)
{
    if (audio_track == NULL)
        return;
    apkenv_audiotrack_write(audio_track, data, bytes);
    audio_bytes += (unsigned long)bytes;
    audio_writes++;
    /* Heartbeat every ~10 s of audio. Bytes-per-second tells you the pump is
     * keeping up with real time (44100*2*2 = 176400 B/s), and the underrun
     * delta tells you the ring never ran dry. A single "WriteData #1" line
     * proves only that the engine started, not that sound keeps flowing. */
    {
        static unsigned long next_report = 0, last_underrun = 0;
        unsigned long secs = audio_bytes / (MORTAR_AUDIO_RATE * MORTAR_AUDIO_CHANNELS * 2);
        if (audio_writes == 1 || secs >= next_report) {
            unsigned long u = apkenv_audiotrack_underrun_bytes(audio_track);
            fprintf(stderr, "[MORTAR-AUDIO] %lu writes, %lu bytes (%lus of audio), "
                    "underrun +%lu\n", audio_writes, audio_bytes, secs, u - last_underrun);
            last_underrun = u;
            next_report = secs + 10;
        }
    }
}

/* ── dialogs: the PvZ-shaped freeze risk ───────────────────────────────────
 * Java's MortarDialog.createDialog posts to the UI thread and returns true;
 * the engine then waits for ButtonWasPressedNative(id, button). Nothing here
 * can show a dialog, so answer it — with the engine's OWN registered native,
 * looked up in apkenv's RegisterNatives table.
 *
 * Button numbering, read off MortarDialog$3's listeners (do not guess it):
 *   setPositiveButton("buttonOne") -> ButtonWasPressed(id, 1)
 *   setNegativeButton("buttonTwo") -> ButtonWasPressed(id, 0)
 *   onCancel (back / tap outside)  -> NativeGameLib.confirmQuitRequest(false)
 * So **0 is the negative/"no" answer** and is the safe default: the one dialog
 * we can identify in this build is the quit confirmation, and 1 would quit the
 * game by itself. Override with APKENV_MORTAR_DIALOG_BUTTON=1 (or -1 to leave
 * dialogs unanswered, which is how to tell a freeze-on-dialog from a freeze
 * somewhere else). */
/* Set by createDialog, cleared by showDialog/removeDialog. A dialog that is
 * created and never shown would otherwise sit unanswered forever, so
 * mortar_update() answers it after a grace period and says so. */
static int pending_dialog = -1;
static unsigned long pending_dialog_frame = 0;

static int
mortar_dialog_button(void)
{
    static int cached = -2;
    if (cached == -2) {
        const char *e = getenv("APKENV_MORTAR_DIALOG_BUTTON");
        cached = e ? atoi(e) : 0;
    }
    return cached;
}

static void
mortar_answer_dialog(int id)
{
    typedef void (*button_pressed_t)(JNIEnv *, jclass, jint, jint) SOFTFP;
    button_pressed_t fn;
    int button = mortar_dialog_button();

    if (button < 0) {
        fprintf(stderr, "[MORTAR-DIALOG] dialog %d left unanswered "
                "(APKENV_MORTAR_DIALOG_BUTTON<0)\n", id);
        return;
    }
    fn = (button_pressed_t)jnienv_find_native_method(
            "com/halfbrick/mortar/MortarDialog", "ButtonWasPressedNative");
    if (fn == NULL)
        fn = (button_pressed_t)jnienv_find_native_method(NULL, "ButtonWasPressedNative");
    if (fn == NULL) {
        fprintf(stderr, "[MORTAR-DIALOG] dialog %d: ButtonWasPressedNative is not "
                "in the RegisterNatives table — the engine may wait forever\n", id);
        return;
    }
    fprintf(stderr, "[MORTAR-DIALOG] answering dialog %d with button %d (%s)\n",
            id, button, button ? "positive" : "negative");
    fn(ENV(global), (jclass)&dialog_class, id, button);
}

/* ── fake-JNI overrides ─────────────────────────────────────────────────────
 * Note: only the ...V (va_list) forms are overridden, and EVERY V form the
 * engine can reach is covered — a missing V override falls through to
 * jni/jnienv.c, whose CallObjectMethodV returns the GLOBAL_J sentinel and
 * turns an unanswered host call into a SIGSEGV in strlen (playbook §4). */

/* The engine binds its own callbacks (MortarDialog.ButtonWasPressedNative,
 * NativeGameLib.native_threadEntry, the ad/billing result callbacks) through
 * RegisterNatives. Log what actually lands in the table: fn-02 looked up
 * native_threadEntry and found nothing, and the table is the only place that
 * can say whether it was registered late, under another class, or not at all. */
static jint
mortar_RegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n)
{
    struct dummy_jclass *c = clazz;
    jint i;
    for (i = 0; i < n; i++)
        fprintf(stderr, "[MORTAR-REG] %s.%s%s -> %p\n", c ? c->name : "?",
                methods[i].name, methods[i].signature, methods[i].fnPtr);
    return JNIEnv_RegisterNatives(env, clazz, methods, n);
}

static jclass
mortar_FindClass(JNIEnv *env, const char *name)
{
    struct dummy_jclass *c = malloc(sizeof(*c));
    c->name = strdup(name);
    return c;
}

static jclass
mortar_GetObjectClass(JNIEnv *env, jobject obj)
{
    struct dummy_jclass *c = malloc(sizeof(*c));
    /* Our objects carry their class name in their first field. */
    c->name = strdup(obj == (jobject)&audio_mixer_obj
                     ? audio_mixer_obj.name : "java/lang/Object");
    return c;
}

/* ── HttpClient: an object with public fields the engine polls ──────────────
 * Java's HttpClient runs the request on its own Thread and publishes the
 * result in public fields; the engine spins on IsFinished and then reads
 * ResponseCode / Result / ReturnedHeaders. There is no network here, so
 * complete the request immediately as "no response" — the shape a real
 * offline device produces, and the one the game already has a screen for
 * ("Ensure you have an active internet connection").
 *
 * Returning NULL from NewObject instead left the engine polling a field id of
 * 0 forever: 2845 GetFieldID(IsFinished) calls in the first 45 s. */
struct mortar_http {
    char *name;                 /* dummy_jclass-compatible */
    int   is_finished;
    int   response_code;
    int   request_pointer;
    struct dummy_array  result;
    struct dummy_jstring headers;
    unsigned char result_byte;
    unsigned long finish_at_frame;   /* 0 = not pending */
    struct mortar_http *next_pending;
};

static struct mortar_http *http_pending = NULL;

static struct mortar_http *
mortar_http_new(void)
{
    struct mortar_http *h = calloc(1, sizeof(*h));
    h->name = (char *)"com/halfbrick/mortar/HttpClient";
    h->result.data = &h->result_byte;
    h->result.element_size = 1;
    h->result.length = 0;
    /* The Java failure path never assigns ReturnedHeaders, so it stays null;
     * GetObjectField returns NULL for it until a request succeeds. */
    return h;
}

static int
mortar_is_http(jobject obj)
{
    struct mortar_object *o = (struct mortar_object *)obj;
    return obj != NULL && o->name != NULL &&
           strcmp(o->name, "com/halfbrick/mortar/HttpClient") == 0;
}

static jboolean
mortar_GetBooleanField(JNIEnv *env, jobject obj, jfieldID field)
{
    if (mortar_is_http(obj) && field && strcmp(FIELD_NAME(field), "IsFinished") == 0)
        return ((struct mortar_http *)obj)->is_finished ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

static jint
mortar_GetIntField(JNIEnv *env, jobject obj, jfieldID field)
{
    if (mortar_is_http(obj) && field) {
        struct mortar_http *h = (struct mortar_http *)obj;
        if (strcmp(FIELD_NAME(field), "ResponseCode") == 0) return h->response_code;
        if (strcmp(FIELD_NAME(field), "RequestPointer") == 0) return h->request_pointer;
    }
    return 0;
}

static jobject
mortar_GetObjectField(JNIEnv *env, jobject obj, jfieldID field)
{
    if (mortar_is_http(obj) && field) {
        struct mortar_http *h = (struct mortar_http *)obj;
        if (strcmp(FIELD_NAME(field), "Result") == 0) return (jobject)&h->result;
        if (strcmp(FIELD_NAME(field), "ReturnedHeaders") == 0)
            return h->headers.data ? (jobject)&h->headers : NULL;
    }
    return NULL;
}

static jthrowable mortar_ExceptionOccurred(JNIEnv *env) { return NULL; }
static void       mortar_ExceptionClear(JNIEnv *env) { }
static jboolean   mortar_ExceptionCheck(JNIEnv *env) { return JNI_FALSE; }

static jobject
mortar_NewObjectV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    struct dummy_jclass *c = clazz;
    if (c && strcmp(c->name, "com/halfbrick/mortar/MortarAudioMixerOut") == 0) {
        fprintf(stderr, "[MORTAR-AUDIO] MortarAudioMixerOut.%s -> AudioTrack %d/%d %s\n",
                method->name, MORTAR_AUDIO_RATE, MORTAR_AUDIO_CHANNELS,
                audio_track ? "(open)" : "(NOT OPEN)");
        return (jobject)&audio_mixer_obj;
    }
    if (c && (strcmp(c->name, "com/halfbrick/mortar/YoutubeEmbedded") == 0 ||
              strcmp(c->name, "com/halfbrick/mortar/WebViewEmbedded") == 0)) {
        /* No web view here, but the engine keeps the reference and calls
         * instance methods on it; NULL would come back through GetObjectClass
         * and friends as a sentinel. */
        struct mortar_object *o = malloc(sizeof(*o));
        o->name = strdup(c->name);
        fprintf(stderr, "[MORTAR-JNI] new %s -> inert object\n", c->name);
        return (jobject)o;
    }
    if (c && strcmp(c->name, "com/halfbrick/mortar/HttpClient") == 0) {
        struct mortar_http *h = mortar_http_new();
        fprintf(stderr, "[MORTAR-HTTP] new HttpClient -> %p\n", (void *)h);
        return (jobject)h;
    }
    fprintf(stderr, "[MORTAR-JNI] UNHANDLED new %s.%s\n", c ? c->name : "?", method->name);
    return NULL;
}

static jobject
mortar_CallStaticObjectMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    {   struct dummy_jclass *c = clazz;
        if (method_is(Create) && c &&
            strcmp(c->name, "com/halfbrick/mortar/MortarAudioMixerOut") == 0)
            return mortar_NewObjectV(env, clazz, method, args);
    }

    /* HBSupport device identity — all read-only facts about the host. */
    if (method_is(GetPackageName))     return (*env)->NewStringUTF(env, "com.halfbrick.fruitninjafree");
    if (method_is(GetPackageVersion))  return (*env)->NewStringUTF(env, "1.8.8");
    if (method_is(GetAndroidVersion))  return (*env)->NewStringUTF(env, "2.3.6");
    if (method_is(GetModel))           return (*env)->NewStringUTF(env, "TouchPad");
    if (method_is(GetManufacturer))    return (*env)->NewStringUTF(env, "HP");
    if (method_is(GetCountry))         return (*env)->NewStringUTF(env, "US");
    if (method_is(GetDeviceLanguage))  return (*env)->NewStringUTF(env, "en");
    if (method_is(GetDeviceLocale))    return (*env)->NewStringUTF(env, "en_US");
    if (method_is(GetUUID) || method_is(GetAndroidID) || method_is(GetDeviceID))
        return (*env)->NewStringUTF(env, "apkenvwebos000000");
    if (method_is(GetGoogleAccount) || method_is(GetAccountEmails))
        return (*env)->NewStringUTF(env, "");

    if (method_is(Encrypt) || method_is(Decrypt)) {
        struct dummy_array *in = va_arg(args, struct dummy_array *);
        struct dummy_array *out = hb_crypt(in, method_is(Encrypt));
        fprintf(stderr, "[MORTAR-CRYPT] %s %ld bytes -> %s\n", method->name,
                in ? in->length * in->element_size : 0, out ? "ok" : "NULL");
        return out;
    }

    if (method_is(GetValue)) {                 /* KeyStore.GetValue(String) */
        char *k = dup_jstring(global, va_arg(args, jstring *));
        const char *v = store_get(&keystore_store, k);
        fprintf(stderr, "[MORTAR-KS] get '%s' -> %s\n", k, v ? v : "(none)");
        free(k);
        return v ? (*env)->NewStringUTF(env, v) : NULL;
    }

    if (method_is(GetRewardName) || method_is(getSettingDescription) ||
        method_is(ExtractDeviceToken) || method_is(ExtractNotificationMessage) ||
        method_is(ExtractNotificationOptionalData) ||
        method_is(ExtractNotificationPushHash) || method_is(ExtractNotificationSplitId))
        return (*env)->NewStringUTF(env, "");

    mortar_trace_unhandled("obj", method);
    return NULL;
}

static jobject
mortar_CallObjectMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    mortar_trace_unhandled("obj(inst)", method);
    return NULL;
}

static jint
mortar_CallStaticIntMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    /* Screen facts the game shapes its UI with. TouchPad: 1024x768 on 9.7",
     * ~132 dpi. Java computes IsDeviceTablet from the diagonal (>= 6.5"). */
    if (method_is(GetDensityDPIType))             return 160;   /* DENSITY_MEDIUM */
    if (method_is(IsDeviceTablet))                return 1;
    if (method_is(GetPhysicalScreenSizeTypeMask)) return 0x14;  /* XLARGE | LONG_NO */
    if (method_is(GetTouchscreenCapabilities))    return 2;     /* multitouch distinct */
    if (method_is(GetWifi))                       return 0;     /* offline */
    if (method_is(GetAdWidth) || method_is(GetAdHeight)) return 0;
    if (method_is(GetRewardAmount))               return 0;
    if (method_is(getSettingControl) || method_is(getSettingValue)) return 0;
    mortar_trace_unhandled("int", method);
    return 0;
}

static jint
mortar_CallIntMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    mortar_trace_unhandled("int(inst)", method);
    return 0;
}

static jboolean
mortar_CallStaticBooleanMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    /* THE audio decision: no OpenSL ES on webOS, so send the engine down its
     * own-mixer / MortarAudioMixerOut path (plan/FRUITNINJA.md §2c). */
    if (method_is(SupportsOpenSL)) {
        static int once = 0;
        if (!once++)
            fprintf(stderr, "[MORTAR] SupportsOpenSL -> 0 "
                    "(webOS has no OpenSL; using the Java mixer path)\n");
        return JNI_FALSE;
    }

    if (method_is(createDialog)) {
        int id = va_arg(args, int);
        fprintf(stderr, "[MORTAR-DIALOG] createDialog(%d) -> true (will auto-answer)\n", id);
        pending_dialog = id;
        pending_dialog_frame = mortar_frames;
        return JNI_TRUE;
    }

    if (method_is(GetBoolPreference)) {
        char *k = dup_jstring(global, va_arg(args, jstring *));
        const char *v = store_get(&prefs_store, k);
        jboolean r = (v && v[0] == '1') ? JNI_TRUE : JNI_FALSE;
        fprintf(stderr, "[MORTAR-PREF] get '%s' -> %d\n", k, r);
        free(k);
        return r;
    }
    if (method_is(PreferenceKeyExists)) {
        char *k = dup_jstring(global, va_arg(args, jstring *));
        jboolean r = store_get(&prefs_store, k) != NULL;
        free(k);
        return r;
    }
    if (method_is(SetValue) || method_is(SetValueIf)) {   /* KeyStore */
        char *k = dup_jstring(global, va_arg(args, jstring *));
        char *v = dup_jstring(global, va_arg(args, jstring *));
        /* SetValueIf(key, value, expected): only store when it matches. */
        int ok = 1;
        if (method_is(SetValueIf)) {
            char *expect = dup_jstring(global, va_arg(args, jstring *));
            const char *cur = store_get(&keystore_store, k);
            ok = (cur == NULL) ? (expect[0] == '\0') : (strcmp(cur, expect) == 0);
            free(expect);
        }
        if (ok) store_set(&keystore_store, k, v);
        fprintf(stderr, "[MORTAR-KS] set '%s' = '%s' -> %d\n", k, v, ok);
        free(k); free(v);
        return ok ? JNI_TRUE : JNI_FALSE;
    }

    /* Services this device does not have. isOnline == false is what stops the
     * engine waiting on HTTP/leaderboards. */
    if (method_is(isOnline) || method_is(IsOnline) || method_is(IsLoggedIn) ||
        method_is(IsBillingSupported) || method_is(InitiatePurchase) ||
        method_is(SupportsBannerAd) || method_is(SupportsBannerAds) ||
        method_is(SupportsFullscreenAd) || method_is(IsBannerAdLoaded) ||
        method_is(IsAdSkipped) || method_is(CheckIfRewardedAdIsAvailable) ||
        method_is(RewardRedeemed) || method_is(showWebHtml) ||
        method_is(UpdateKeyboard) || method_is(setSettingValue))
        return JNI_FALSE;

    mortar_trace_unhandled("bool", method);
    return JNI_FALSE;
}

static jboolean
mortar_CallBooleanMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    mortar_trace_unhandled("bool(inst)", method);
    return JNI_FALSE;
}

static void
mortar_CallStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    if (method_is(Init)) {                     /* HBSupport.Init(key, iv) */
        struct dummy_jclass *c = clazz;
        if (c && strcmp(c->name, "com/halfbrick/mortar/HBSupport") == 0) {
            char *k = dup_jstring(global, va_arg(args, jstring *));
            char *v = dup_jstring(global, va_arg(args, jstring *));
            hb_keylen = (int)strlen(k); if (hb_keylen > 32) hb_keylen = 32;
            hb_ivlen  = (int)strlen(v); if (hb_ivlen  > 16) hb_ivlen  = 16;
            memcpy(hb_key, k, (size_t)hb_keylen);
            memcpy(hb_iv,  v, (size_t)hb_ivlen);
            fprintf(stderr, "[MORTAR-CRYPT] HBSupport.Init keylen=%d ivlen=%d\n",
                    hb_keylen, hb_ivlen);
            free(k); free(v);
            return;
        }
    }

    /* The engine creates a worker with pthread_create, then has the trampoline
     * call this Java static so the thread is attached to the VM — and the Java
     * static is itself one of the engine's RegisterNatives entries. Dropping it
     * silently makes the thread exit immediately (its "<<< end ... ret=(nil)"
     * lands one line after the tracer's UNHANDLED). Dispatch it to the engine's
     * own implementation, on this thread, which is where it belongs. */
    if (method_is(native_threadEntry)) {
        typedef void (*thread_entry_t)(JNIEnv *, jclass, jint) SOFTFP;
        int id = va_arg(args, int);
        thread_entry_t fn = (thread_entry_t)jnienv_find_native_method(
                "com/halfbrick/mortar/NativeGameLib", "native_threadEntry");
        if (fn == NULL)   /* class name may differ; the method name is unique */
            fn = (thread_entry_t)jnienv_find_native_method(NULL, "native_threadEntry");
        fprintf(stderr, "[MORTAR-THREAD] native_threadEntry(%d) -> %s\n",
                id, fn ? "dispatching" : "NOT REGISTERED");
        if (fn) fn(env, clazz, id);
        fprintf(stderr, "[MORTAR-THREAD] native_threadEntry(%d) returned\n", id);
        return;
    }

    if (method_is(SetBoolPreference)) {
        char *k = dup_jstring(global, va_arg(args, jstring *));
        int v = va_arg(args, int);
        fprintf(stderr, "[MORTAR-PREF] set '%s' = %d\n", k, v);
        store_set(&prefs_store, k, v ? "1" : "0");
        free(k);
        return;
    }

    if (method_is(showDialog)) {
        int id = va_arg(args, int);
        fprintf(stderr, "[MORTAR-DIALOG] showDialog(%d)\n", id);
        /* Answer it from inside the call: the engine has already published the
         * dialog and cleared its result slot in createDialog, so there is no
         * race with the poll that follows. */
        mortar_answer_dialog(id);
        pending_dialog = -1;
        return;
    }
    if (method_is(removeDialog)) {
        pending_dialog = -1;
        return;
    }

    if (method_is(SetScreenResolution)) {
        int w = va_arg(args, int), h = va_arg(args, int);
        fprintf(stderr, "[MORTAR] SetScreenResolution(%d, %d)\n", w, h);
        return;
    }

    /* Fire-and-forget host services that simply do not exist here. Logged
     * once each by the tracer so a surprise shows up in the device log. */
    mortar_trace_unhandled("void", method);
}

static void
mortar_CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    if (method_is(WriteData)) {
        struct dummy_array *a = va_arg(args, struct dummy_array *);
        if (a && a->data && a->length > 0)
            mortar_audio_write(a->data, (int)(a->length * a->element_size));
        return;
    }
    if (method_is(FinishedCopyingDataJNI) && mortar_is_http(obj)) {
        /* Java sets MarkForDelete here: the engine has copied Result out and
         * will not touch this client again. The game issues roughly one ad-media
         * request a second forever, so not freeing them does add up. */
        struct mortar_http *h = (struct mortar_http *)obj, **pp = &http_pending;
        while (*pp) { if (*pp == h) { *pp = h->next_pending; break; } pp = &(*pp)->next_pending; }
        free(h);
        return;
    }

    if (method_is(HttpRequest) && mortar_is_http(obj)) {
        /* HttpRequest(verb, url, headers, body, requestPointer, timeoutSeconds) */
        struct mortar_http *h = (struct mortar_http *)obj;
        char *verb = dup_jstring(global, va_arg(args, jstring *));
        char *url  = dup_jstring(global, va_arg(args, jstring *));
        (void)va_arg(args, jstring *);            /* headers  */
        (void)va_arg(args, struct dummy_array *); /* body     */
        h->request_pointer = va_arg(args, int);   /* the engine's request ctx */
        (void)va_arg(args, int);                  /* timeoutSeconds */

        /* Reproduce the Java worker's FAILURE path exactly (HttpClient$1
         * catch_4e / catch_e4): Result = new byte[0], IsFinished = true,
         * ResponseCode left at 0, ReturnedHeaders left null. And reproduce it
         * ASYNCHRONOUSLY: the Java side always finishes on another thread,
         * after HttpRequest() has returned and the engine has finished
         * registering the request. Completing inside the call instead let the
         * engine parse a response for a request it had not finished setting
         * up, and it dereferenced NULL in its NetworkManager (fn-02). */
        h->result.length = 0;
        h->headers.data = NULL;
        h->response_code = 0;
        h->finish_at_frame = mortar_frames + 30;   /* ~1 s at 30 fps */
        h->next_pending = http_pending;
        http_pending = h;
        fprintf(stderr, "[MORTAR-HTTP] %s %s (ptr=%d) -> offline, will fail at frame %lu\n",
                verb ? verb : "?", url ? url : "?", h->request_pointer,
                h->finish_at_frame);
        free(verb); free(url);
        return;
    }
    mortar_trace_unhandled("void(inst)", method);
}

/* ── module ────────────────────────────────────────────────────────────────── */

static int
mortar_try_init(struct SupportModule *self)
{
    int found = 0, want = 0;
#define GET(field, sym, type) do {                                          \
        self->priv->field = (type)LOOKUP_M(sym);                            \
        want++;                                                             \
        if (self->priv->field) found++;                                     \
    } while (0)

    /* libmortargame exports JNI_OnLoad (0x239fe0). Dalvik runs it at
     * System.loadLibrary time, and it is where the engine RegisterNatives its
     * own callbacks — native_threadEntry, MortarDialog.ButtonWasPressedNative,
     * the ad/billing result natives. Skipping it left every one of those
     * unbound, which showed up as a worker thread that called
     * native_threadEntry and exited immediately (fn-02/fn-03). Resolve it from
     * the game lib by name: the apk also ships libMicroMapJNI.so, which has a
     * JNI_OnLoad of its own. */
    self->priv->JNI_OnLoad = (jni_onload_t)LOOKUP_LIBM("libmortargame", "JNI_OnLoad");
    want++;
    if (self->priv->JNI_OnLoad) found++;

    GET(native_SystemInit,            "_NativeGameLib_native_1SystemInit",            mortar_systeminit_t);
    GET(native_GameInit,              "_NativeGameLib_native_1GameInit",              mortar_gameinit_t);
    GET(native_InitFileManager,       "_NativeGameLib_native_1InitFileManager",       mortar_initfilemanager_t);
    GET(native_InitJavaSoundManager,  "_NativeGameLib_native_1InitJavaSoundManager",  mortar_initjavasound_t);
    GET(native_InitOpenSLSoundManager,"_NativeGameLib_native_1InitOpenSLSoundManager",mortar_initopenslsound_t);
    GET(native_step,                  "_NativeGameLib_native_1step",                  mortar_step_t);
    GET(native_touchEvent,            "_NativeGameLib_native_1touchEvent",            mortar_touchevent_t);
    GET(native_keyEvent,              "_NativeGameLib_native_1keyEvent",              mortar_keyevent_t);
    GET(native_onPause,               "_NativeGameLib_native_1onPause",               mortar_void_t);
    GET(native_onResume,              "_NativeGameLib_native_1onResume",              mortar_void_t);
    GET(native_onFocusLost,           "_NativeGameLib_native_1onFocusLost",           mortar_void_t);
    GET(native_onFocusRetrieved,      "_NativeGameLib_native_1onFocusRetrieved",      mortar_void_t);
    GET(native_saveOnExit,            "_NativeGameLib_native_1saveOnExit",            mortar_void_t);
    GET(native_gameRequestedQuit,     "_NativeGameLib_native_1gameRequestedQuit",     mortar_bool_t);
    GET(native_gameRequestedRestart,  "_NativeGameLib_native_1gameRequestedRestart",  mortar_bool_t);
    GET(native_SetAppLicensed,        "_NativeGameLib_native_1SetAppLicensed",        mortar_setapplicensed_t);
#undef GET

    /* The 1.8.x generation is identified by SystemInit + GameInit existing as
     * a pair; 1.7.x had a single native_init and would fail here. */
    if (self->priv->native_SystemInit == NULL || self->priv->native_GameInit == NULL ||
        self->priv->native_step == NULL || self->priv->native_touchEvent == NULL ||
        self->priv->native_InitFileManager == NULL)
        return 0;

    fprintf(stderr, "[MORTAR] try_init: found %d/%d natives (Mortar 1.8.x generation)\n",
            found, want);

    /* Landscape game on a landscape device: no rotation, no FBO. The engine is
     * ES2-only (DT_NEEDED is libGLESv2.so alone) — say so before the loader
     * runs its own heuristic. */
    GLOBAL_M->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;
    GLOBAL_M->module_hacks->prefer_gles_version = 2;

    self->override_env.RegisterNatives           = mortar_RegisterNatives;
    self->override_env.FindClass                 = mortar_FindClass;
    self->override_env.GetObjectClass            = mortar_GetObjectClass;
    self->override_env.GetBooleanField           = mortar_GetBooleanField;
    self->override_env.GetIntField               = mortar_GetIntField;
    self->override_env.GetObjectField            = mortar_GetObjectField;
    self->override_env.ExceptionOccurred         = mortar_ExceptionOccurred;
    self->override_env.ExceptionClear            = mortar_ExceptionClear;
    self->override_env.ExceptionCheck            = mortar_ExceptionCheck;
    self->override_env.NewObjectV                = mortar_NewObjectV;
    self->override_env.CallStaticObjectMethodV   = mortar_CallStaticObjectMethodV;
    self->override_env.CallObjectMethodV         = mortar_CallObjectMethodV;
    self->override_env.CallStaticIntMethodV      = mortar_CallStaticIntMethodV;
    self->override_env.CallIntMethodV            = mortar_CallIntMethodV;
    self->override_env.CallStaticBooleanMethodV  = mortar_CallStaticBooleanMethodV;
    self->override_env.CallBooleanMethodV        = mortar_CallBooleanMethodV;
    self->override_env.CallStaticVoidMethodV     = mortar_CallStaticVoidMethodV;
    self->override_env.CallVoidMethodV           = mortar_CallVoidMethodV;

    return 1;
}

static void
mortar_init(struct SupportModule *self, int width, int height, const char *home)
{
    char cache[PATH_MAX];
    global = GLOBAL_M;

    if (self->priv->JNI_OnLoad) {
        fprintf(stderr, "[MORTAR] JNI_OnLoad\n");
        self->priv->JNI_OnLoad(VM_M, NULL);
    } else {
        fprintf(stderr, "[MORTAR] no JNI_OnLoad in libmortargame — engine "
                "callbacks will not be registered\n");
    }

    snprintf(self->priv->home, sizeof(self->priv->home), "%s", home);
    self->priv->screen_w = width;
    self->priv->screen_h = height;
    snprintf(cache, sizeof(cache), "%scache/", home);
    global->recursive_mkdir(cache);

    /* Open the PCM sink here, on the main thread: the engine's own "Audio
     * Thread" is what calls WriteData, and SDL_OpenAudio from there is a
     * needless risk. Rate/format are fixed by MortarAudioMixerOut's ctor. */
    mortar_audio_open();

    /* --- GameManager.SystemInit(gl), in the host's own order --- */
    glFrontFace(GL_CW);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);

    fprintf(stderr, "[MORTAR] InitFileManager(apk=%s, files=%s, cache=%s)\n",
            global->apk_filename, home, cache);
    self->priv->native_InitFileManager(ENV_M, GLOBAL_M,
            (*ENV_M)->NewStringUTF(ENV_M, global->apk_filename),
            (*ENV_M)->NewStringUTF(ENV_M, home),
            (*ENV_M)->NewStringUTF(ENV_M, cache),
            JNI_FALSE);

    if (self->priv->native_InitOpenSLSoundManager) {
        /* The engine checks SupportsOpenSL() (we answer 0) before it looks at
         * the AssetManager, but hand it a real object rather than NULL so a
         * reordered build cannot fault on it. */
        static struct dummy_jclass asset_manager =
            { (char *)"android/content/res/AssetManager" };
        jboolean sl = self->priv->native_InitOpenSLSoundManager(ENV_M, GLOBAL_M,
                (jobject)&asset_manager);
        fprintf(stderr, "[MORTAR] InitOpenSLSoundManager -> %d\n", sl);
        if (!sl && self->priv->native_InitJavaSoundManager) {
            fprintf(stderr, "[MORTAR] InitJavaSoundManager\n");
            self->priv->native_InitJavaSoundManager(ENV_M, GLOBAL_M);
        }
    }

    fprintf(stderr, "[MORTAR] SystemInit(%d, %d, \"en\")\n", width, height);
    self->priv->native_SystemInit(ENV_M, GLOBAL_M, width, height,
            (*ENV_M)->NewStringUTF(ENV_M, "en"));

    /* doLicenseCheck() is EMPTY in the free build, so the host never calls
     * SetAppLicensed. Opt in if a paid build (or ad removal) needs it. */
    {
        const char *lic = getenv("APKENV_MORTAR_LICENSED");
        if (lic && lic[0] == '1' && self->priv->native_SetAppLicensed) {
            fprintf(stderr, "[MORTAR] SetAppLicensed(1) (APKENV_MORTAR_LICENSED)\n");
            self->priv->native_SetAppLicensed(ENV_M, GLOBAL_M, JNI_TRUE);
        }
    }
}

/* ── APKENV_MORTAR_AUTOTAP: synthesise input, for testing without a finger ──
 * Entries separated by ';':
 *     "x,y@frame"            tap: press at `frame`, release 6 frames later
 *     "x1,y1>x2,y2@frame"    swipe: press, 8 interpolated moves, release
 * Coordinates are screen pixels. Everything goes through mortar_input(), so it
 * exercises the real contract (normalized 0..1, Android action codes) rather
 * than a shortcut. Fruit Ninja is swipe-driven — "SLICE FRUIT TO BEGIN" — so a
 * tap-only probe would prove only half of it. Diagnostic: pair with
 * tools/grab.sh, and never set this in a shipped env file. */
#define MORTAR_AUTOTAP_MAX 24
#define MORTAR_SWIPE_STEPS 8
struct autotap_entry {
    int x0, y0, x1, y1;
    int is_swipe;
    unsigned long frame;
    int stage;                  /* 0 = pending, 1..n = in progress, -1 = done */
};
static struct autotap_entry autotap[MORTAR_AUTOTAP_MAX];
static int autotap_n = -1;

static void
mortar_autotap_parse(void)
{
    const char *e = getenv("APKENV_MORTAR_AUTOTAP");
    autotap_n = 0;
    if (e == NULL || e[0] == '\0') return;
    while (*e && autotap_n < MORTAR_AUTOTAP_MAX) {
        struct autotap_entry *a = &autotap[autotap_n];
        int x0, y0, x1, y1;
        unsigned long f;
        if (sscanf(e, "%d,%d>%d,%d@%lu", &x0, &y0, &x1, &y1, &f) == 5) {
            a->x0 = x0; a->y0 = y0; a->x1 = x1; a->y1 = y1;
            a->is_swipe = 1; a->frame = f; a->stage = 0;
            fprintf(stderr, "[MORTAR-AUTOTAP] queued swipe (%d,%d)->(%d,%d) at frame %lu\n",
                    x0, y0, x1, y1, f);
            autotap_n++;
        } else if (sscanf(e, "%d,%d@%lu", &x0, &y0, &f) == 3) {
            a->x0 = a->x1 = x0; a->y0 = a->y1 = y0;
            a->is_swipe = 0; a->frame = f; a->stage = 0;
            fprintf(stderr, "[MORTAR-AUTOTAP] queued tap (%d,%d) at frame %lu\n", x0, y0, f);
            autotap_n++;
        }
        e = strchr(e, ';');
        if (e == NULL) break;
        e++;
    }
}

static void mortar_input(struct SupportModule *self, int event, int x, int y, int finger);

static void
mortar_autotap_run(struct SupportModule *self)
{
    int i;
    if (autotap_n < 0) mortar_autotap_parse();
    for (i = 0; i < autotap_n; i++) {
        struct autotap_entry *a = &autotap[i];
        int last = MORTAR_SWIPE_STEPS + 1;   /* a tap is a swipe of zero length */
        long step;
        if (a->stage < 0) continue;
        if (a->stage == 0) {
            if (mortar_frames != a->frame) continue;
            fprintf(stderr, "[MORTAR-AUTOTAP] down (%d,%d)\n", a->x0, a->y0);
            mortar_input(self, ACTION_DOWN, a->x0, a->y0, 0);
            a->stage = 1;
            continue;
        }
        /* one event per frame after the press */
        step = (long)(mortar_frames - a->frame);
        if (step != a->stage) continue;
        if (a->stage < last) {
            int x = a->x0 + (a->x1 - a->x0) * a->stage / MORTAR_SWIPE_STEPS;
            int y = a->y0 + (a->y1 - a->y0) * a->stage / MORTAR_SWIPE_STEPS;
            mortar_input(self, ACTION_MOVE, x, y, 0);
            a->stage++;
        } else {
            fprintf(stderr, "[MORTAR-AUTOTAP] up   (%d,%d)\n", a->x1, a->y1);
            mortar_input(self, ACTION_UP, a->x1, a->y1, 0);
            a->stage = -1;
        }
    }
}

/* Android MotionEvent action codes the Java host forwards verbatim. */
#define AMOTION_POINTER_DOWN 5
#define AMOTION_POINTER_UP   6
#define MORTAR_MAX_FINGERS   10

static int finger_down[MORTAR_MAX_FINGERS];
static unsigned long touch_logged = 0;

static void
mortar_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    int action = event, i, others = 0;
    float fx, fy;

    if (finger < 0 || finger >= MORTAR_MAX_FINGERS) finger = 0;

    /* MultiTouchInputHandler forwards MotionEvent.getAction() unchanged, so
     * a second finger arrives as ACTION_POINTER_DOWN|index<<8, not ACTION_DOWN. */
    for (i = 0; i < MORTAR_MAX_FINGERS; i++)
        if (i != finger && finger_down[i]) others++;

    if (event == ACTION_DOWN) {
        if (others) action = AMOTION_POINTER_DOWN | (finger << 8);
        finger_down[finger] = 1;
    } else if (event == ACTION_UP) {
        if (others) action = AMOTION_POINTER_UP | (finger << 8);
        finger_down[finger] = 0;
    }

    /* NORMALIZED 0..1 — getX(i)/display.getWidth() in the Java host. */
    fx = (float)x / (float)self->priv->screen_w;
    fy = (float)y / (float)self->priv->screen_h;

    if (touch_logged++ < 40)
        fprintf(stderr, "[MORTAR-TOUCH] ev=%d finger=%d raw=(%d,%d) -> action=0x%x (%.3f,%.3f)\n",
                event, finger, x, y, action, fx, fy);

    self->priv->native_touchEvent(ENV_M, GLOBAL_M, action, 0, finger, fx, fy, 1.0f, 0.1f);
}

static void
mortar_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    if (self->priv->native_keyEvent)
        self->priv->native_keyEvent(ENV_M, GLOBAL_M, keycode,
                event == ACTION_DOWN ? JNI_TRUE : JNI_FALSE, JNI_FALSE);
}

static void
mortar_update(struct SupportModule *self)
{
    /* First frame: GameManager.Render's callers have already run SystemInit;
     * with no splash the host runs GameInit in the same frame, before Render. */
    if (!self->priv->game_initialized) {
        fprintf(stderr, "[MORTAR] GameInit\n");
        self->priv->native_GameInit(ENV_M, GLOBAL_M);
        self->priv->game_initialized = 1;
    }

    /* GameManager.Render's per-frame GL state. */
    glFrontFace(GL_CW);
    glEnable(GL_DITHER);
    glCullFace(GL_BACK);
    glEnable(GL_DEPTH_TEST);

    /* Retire HTTP requests the way the Java worker thread would. */
    {
        struct mortar_http **pp = &http_pending;
        while (*pp) {
            struct mortar_http *h = *pp;
            if (mortar_frames >= h->finish_at_frame) {
                h->is_finished = 1;
                h->finish_at_frame = 0;
                fprintf(stderr, "[MORTAR-HTTP] request ptr=%d failed (offline)\n",
                        h->request_pointer);
                *pp = h->next_pending;
                h->next_pending = NULL;
            } else {
                pp = &h->next_pending;
            }
        }
    }

    /* A dialog created but never shown still blocks whatever is polling for
     * its answer; do not let that read as a mystery freeze. */
    if (pending_dialog >= 0 && mortar_frames - pending_dialog_frame > 120) {
        fprintf(stderr, "[MORTAR-DIALOG] dialog %d was created but never shown "
                "(%lu frames) — answering it anyway\n",
                pending_dialog, mortar_frames - pending_dialog_frame);
        mortar_answer_dialog(pending_dialog);
        pending_dialog = -1;
    }

    mortar_autotap_run(self);

    mortar_frames++;
    if (!self->priv->native_step(ENV_M, GLOBAL_M)) {
        fprintf(stderr, "[MORTAR] step -> false at frame %lu: shutting down\n", mortar_frames);
        self->priv->want_exit = 1;
        return;
    }
    if (mortar_frames == 1)
        fprintf(stderr, "[MORTAR] step #1 -> 1\n");

    if (self->priv->native_gameRequestedQuit &&
        self->priv->native_gameRequestedQuit(ENV_M, GLOBAL_M)) {
        fprintf(stderr, "[MORTAR] gameRequestedQuit at frame %lu\n", mortar_frames);
        self->priv->want_exit = 1;
    }
}

static void
mortar_deinit(struct SupportModule *self)
{
    if (self->priv->native_saveOnExit)
        self->priv->native_saveOnExit(ENV_M, GLOBAL_M);
    if (audio_track) {
        apkenv_audiotrack_release(audio_track);
        audio_track = NULL;
    }
}

static void
mortar_pause(struct SupportModule *self)
{
    if (self->priv->native_onFocusLost) self->priv->native_onFocusLost(ENV_M, GLOBAL_M);
    if (self->priv->native_onPause) self->priv->native_onPause(ENV_M, GLOBAL_M);
    if (self->priv->native_saveOnExit) self->priv->native_saveOnExit(ENV_M, GLOBAL_M);
    if (audio_track) apkenv_audiotrack_pause(audio_track);
}

static void
mortar_resume(struct SupportModule *self)
{
    if (audio_track) apkenv_audiotrack_play(audio_track);
    if (self->priv->native_onResume) self->priv->native_onResume(ENV_M, GLOBAL_M);
    if (self->priv->native_onFocusRetrieved) self->priv->native_onFocusRetrieved(ENV_M, GLOBAL_M);
}

static int
mortar_requests_exit(struct SupportModule *self)
{
    return self->priv->want_exit;
}

APKENV_MODULE(mortar, MODULE_PRIORITY_ENGINE)
