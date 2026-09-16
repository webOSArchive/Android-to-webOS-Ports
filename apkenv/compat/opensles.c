/**
 * apkenv — OpenSL ES shim. See opensles.h.
 *
 * ABI. Every SL interface is a pointer to a pointer to a const vtable, and the
 * method ORDER below is the ABI: it is copied from the Khronos OpenSL ES 1.0.1
 * OpenSLES.h and Android's OpenSLES_Android.h, and each table is filled
 * positionally with a comment per slot. The slots FMOD actually uses were
 * checked against libfmodex's disassembly (Tiny Death Star 1.4.1,
 * fmod_output_opensl.cpp):
 *   ObjectItf  +0 Realize, +12 GetInterface
 *   EngineItf  +8 CreateAudioPlayer, +28 CreateOutputMix
 *   AndroidConfigurationItf +0 SetConfiguration("androidPlaybackStreamType", 3)
 *   AndroidSimpleBufferQueueItf +0 Enqueue, +12 RegisterCallback
 * Unused slots still point at real functions that log and fail cleanly — a
 * NULL slot would be a jump to 0.
 *
 * Playback model (Android's): the app enqueues buffers it owns; the player
 * plays them in order and, after each one has been consumed, calls the
 * buffer-queue callback, from which the app enqueues the next. Here a worker
 * thread per player pops a buffer, writes it to an apkenv AudioTrack (whose
 * blocking write paces real time) and then calls back. The queue lock is never
 * held across the write or the callback, so Enqueue from inside the callback
 * cannot deadlock, and the popped buffer is already out of the count, so a
 * queue that was full has room for the refill.
 */
#include "opensles.h"
#include "../audio/audiotrack.h"

#ifndef OPENSLES_UNIT_TEST
#  include "../apkenv.h"     /* SOFTFP */
#else
#  define SOFTFP
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── OpenSL ES 1.0.1 types ─────────────────────────────────────────────────── */
typedef unsigned char  SLuint8;
typedef signed short   SLint16;
typedef unsigned short SLuint16;
typedef signed int     SLint32;
typedef unsigned int   SLuint32;
typedef SLuint32       SLboolean;
typedef SLuint8        SLchar;
typedef SLuint32       SLresult;
typedef SLuint32       SLmillisecond;
typedef SLint16        SLmillibel;
typedef SLint16        SLpermille;

#define SL_BOOLEAN_FALSE ((SLboolean)0x00000000)
#define SL_BOOLEAN_TRUE  ((SLboolean)0x00000001)

#define SL_RESULT_SUCCESS               ((SLuint32)0x00000000)
#define SL_RESULT_PRECONDITIONS_VIOLATED ((SLuint32)0x00000001)
#define SL_RESULT_PARAMETER_INVALID     ((SLuint32)0x00000002)
#define SL_RESULT_MEMORY_FAILURE        ((SLuint32)0x00000003)
#define SL_RESULT_RESOURCE_ERROR        ((SLuint32)0x00000004)
#define SL_RESULT_BUFFER_INSUFFICIENT   ((SLuint32)0x00000007)
#define SL_RESULT_CONTENT_UNSUPPORTED   ((SLuint32)0x00000009)
#define SL_RESULT_FEATURE_UNSUPPORTED   ((SLuint32)0x0000000C)

#define SL_OBJECT_STATE_UNREALIZED ((SLuint32)0x00000001)
#define SL_OBJECT_STATE_SUSPENDED  ((SLuint32)0x00000002)
#define SL_OBJECT_STATE_REALIZED   ((SLuint32)0x00000003)

#define SL_PLAYSTATE_STOPPED ((SLuint32)0x00000001)
#define SL_PLAYSTATE_PAUSED  ((SLuint32)0x00000002)
#define SL_PLAYSTATE_PLAYING ((SLuint32)0x00000003)

#define SL_DATALOCATOR_OUTPUTMIX   ((SLuint32)0x00000004)
#define SL_DATALOCATOR_BUFFERQUEUE ((SLuint32)0x00000006)
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ((SLuint32)0x800007BD)
#define SL_DATAFORMAT_PCM          ((SLuint32)0x00000002)
#define SL_BYTEORDER_BIGENDIAN     ((SLuint32)0x00000001)
#define SL_BYTEORDER_LITTLEENDIAN  ((SLuint32)0x00000002)

struct SLInterfaceID_ {
    SLuint32 time_low;
    SLuint16 time_mid;
    SLuint16 time_hi_and_version;
    SLuint16 clock_seq;
    SLuint8  node[6];
};
typedef const struct SLInterfaceID_ *SLInterfaceID;

struct SLObjectItf_;
typedef const struct SLObjectItf_ * const *SLObjectItf;
struct SLEngineItf_;
typedef const struct SLEngineItf_ * const *SLEngineItf;
struct SLPlayItf_;
typedef const struct SLPlayItf_ * const *SLPlayItf;
struct SLAndroidSimpleBufferQueueItf_;
typedef const struct SLAndroidSimpleBufferQueueItf_ * const *SLAndroidSimpleBufferQueueItf;
struct SLAndroidConfigurationItf_;
typedef const struct SLAndroidConfigurationItf_ * const *SLAndroidConfigurationItf;
struct SLVolumeItf_;
typedef const struct SLVolumeItf_ * const *SLVolumeItf;

typedef struct { SLuint32 feature; SLuint32 data; } SLEngineOption;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;
typedef struct { SLuint32 locatorType; SLuint32 numBuffers; } SLDataLocator_BufferQueue;
typedef struct { SLuint32 locatorType; SLObjectItf outputMix; } SLDataLocator_OutputMix;
typedef struct {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;    /* milliHz */
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;
typedef struct { SLuint32 count; SLuint32 index; } SLAndroidSimpleBufferQueueState;

typedef void (*SLObjectCallback)(SLObjectItf caller, const void *pContext, SLuint32 event,
                                 SLresult result, SLuint32 param, void *pInterface) SOFTFP;
typedef void (*SLPlayCallback)(SLPlayItf caller, void *pContext, SLuint32 event) SOFTFP;
typedef void (*SLAndroidSimpleBufferQueueCallback)(SLAndroidSimpleBufferQueueItf caller,
                                                   void *pContext) SOFTFP;

struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf self, SLboolean async) SOFTFP;
    SLresult (*Resume)(SLObjectItf self, SLboolean async) SOFTFP;
    SLresult (*GetState)(SLObjectItf self, SLuint32 *pState) SOFTFP;
    SLresult (*GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *pInterface) SOFTFP;
    SLresult (*RegisterCallback)(SLObjectItf self, SLObjectCallback callback, void *pContext) SOFTFP;
    void     (*AbortAsyncOperation)(SLObjectItf self) SOFTFP;
    void     (*Destroy)(SLObjectItf self) SOFTFP;
    SLresult (*SetPriority)(SLObjectItf self, SLint32 priority, SLboolean preemptable) SOFTFP;
    SLresult (*GetPriority)(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable) SOFTFP;
    SLresult (*SetLossOfControlInterfaces)(SLObjectItf self, SLint16 numInterfaces,
                                           SLInterfaceID *pInterfaceIDs, SLboolean enabled) SOFTFP;
};

struct SLEngineItf_ {
    SLresult (*CreateLEDDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
            SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
            const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateVibraDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
            SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
            const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateAudioPlayer)(SLEngineItf self, SLObjectItf *pPlayer,
            SLDataSource *pAudioSrc, SLDataSink *pAudioSnk, SLuint32 numInterfaces,
            const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateAudioRecorder)(SLEngineItf self, SLObjectItf *pRecorder,
            SLDataSource *pAudioSrc, SLDataSink *pAudioSnk, SLuint32 numInterfaces,
            const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateMidiPlayer)(SLEngineItf self, SLObjectItf *pPlayer,
            SLDataSource *pMIDISrc, SLDataSource *pBankSrc, SLDataSink *pAudioOutput,
            SLDataSink *pVibra, SLDataSink *pLEDArray, SLuint32 numInterfaces,
            const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateListener)(SLEngineItf self, SLObjectItf *pListener,
            SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
            const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*Create3DGroup)(SLEngineItf self, SLObjectItf *pGroup,
            SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
            const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateOutputMix)(SLEngineItf self, SLObjectItf *pMix,
            SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
            const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateMetadataExtractor)(SLEngineItf self, SLObjectItf *pMetadataExtractor,
            SLDataSource *pDataSource, SLuint32 numInterfaces,
            const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*CreateExtensionObject)(SLEngineItf self, SLObjectItf *pObject,
            void *pParameters, SLuint32 objectID, SLuint32 numInterfaces,
            const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) SOFTFP;
    SLresult (*QueryNumSupportedInterfaces)(SLEngineItf self, SLuint32 objectID,
            SLuint32 *pNumSupportedInterfaces) SOFTFP;
    SLresult (*QuerySupportedInterfaces)(SLEngineItf self, SLuint32 objectID,
            SLuint32 index, SLInterfaceID *pInterfaceId) SOFTFP;
    SLresult (*QueryNumSupportedExtensions)(SLEngineItf self, SLuint32 *pNumExtensions) SOFTFP;
    SLresult (*QuerySupportedExtension)(SLEngineItf self, SLuint32 index,
            SLchar *pExtensionName, SLint16 *pNameLength) SOFTFP;
    SLresult (*IsExtensionSupported)(SLEngineItf self, const SLchar *pExtensionName,
            SLboolean *pSupported) SOFTFP;
};

struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf self, SLuint32 state) SOFTFP;
    SLresult (*GetPlayState)(SLPlayItf self, SLuint32 *pState) SOFTFP;
    SLresult (*GetDuration)(SLPlayItf self, SLmillisecond *pMsec) SOFTFP;
    SLresult (*GetPosition)(SLPlayItf self, SLmillisecond *pMsec) SOFTFP;
    SLresult (*RegisterCallback)(SLPlayItf self, SLPlayCallback callback, void *pContext) SOFTFP;
    SLresult (*SetCallbackEventsMask)(SLPlayItf self, SLuint32 eventFlags) SOFTFP;
    SLresult (*GetCallbackEventsMask)(SLPlayItf self, SLuint32 *pEventFlags) SOFTFP;
    SLresult (*SetMarkerPosition)(SLPlayItf self, SLmillisecond mSec) SOFTFP;
    SLresult (*ClearMarkerPosition)(SLPlayItf self) SOFTFP;
    SLresult (*GetMarkerPosition)(SLPlayItf self, SLmillisecond *pMsec) SOFTFP;
    SLresult (*SetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond mSec) SOFTFP;
    SLresult (*GetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond *pMsec) SOFTFP;
};

struct SLAndroidSimpleBufferQueueItf_ {
    SLresult (*Enqueue)(SLAndroidSimpleBufferQueueItf self, const void *pBuffer, SLuint32 size) SOFTFP;
    SLresult (*Clear)(SLAndroidSimpleBufferQueueItf self) SOFTFP;
    SLresult (*GetState)(SLAndroidSimpleBufferQueueItf self,
                         SLAndroidSimpleBufferQueueState *pState) SOFTFP;
    SLresult (*RegisterCallback)(SLAndroidSimpleBufferQueueItf self,
                                 SLAndroidSimpleBufferQueueCallback callback, void *pContext) SOFTFP;
};

struct SLAndroidConfigurationItf_ {
    SLresult (*SetConfiguration)(SLAndroidConfigurationItf self, const SLchar *configKey,
                                 const void *pConfigValue, SLuint32 valueSize) SOFTFP;
    SLresult (*GetConfiguration)(SLAndroidConfigurationItf self, const SLchar *configKey,
                                 SLuint32 *pValueSize, void *pConfigValue) SOFTFP;
};

struct SLVolumeItf_ {
    SLresult (*SetVolumeLevel)(SLVolumeItf self, SLmillibel level) SOFTFP;
    SLresult (*GetVolumeLevel)(SLVolumeItf self, SLmillibel *pLevel) SOFTFP;
    SLresult (*GetMaxVolumeLevel)(SLVolumeItf self, SLmillibel *pMaxLevel) SOFTFP;
    SLresult (*SetMute)(SLVolumeItf self, SLboolean mute) SOFTFP;
    SLresult (*GetMute)(SLVolumeItf self, SLboolean *pMute) SOFTFP;
    SLresult (*EnableStereoPosition)(SLVolumeItf self, SLboolean enable) SOFTFP;
    SLresult (*IsEnabledStereoPosition)(SLVolumeItf self, SLboolean *pEnable) SOFTFP;
    SLresult (*SetStereoPosition)(SLVolumeItf self, SLpermille stereoPosition) SOFTFP;
    SLresult (*GetStereoPosition)(SLVolumeItf self, SLpermille *pStereoPosition) SOFTFP;
};

/* ── interface IDs (Android's OpenSLES_IID.c values) ───────────────────────── */
static const struct SLInterfaceID_ iid_null   = { 0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, { 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10 } };
static const struct SLInterfaceID_ iid_object = { 0x79216360, 0xddd7, 0x11db, 0xac16, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_engine = { 0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_play   = { 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_record = { 0xc5657aa0, 0xdddb, 0x11db, 0x82f7, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_volume = { 0x09e8ede0, 0xddde, 0x11db, 0xb4f6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_bufferqueue = { 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_outputmix   = { 0x97750f60, 0xddd7, 0x11db, 0x92b1, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_android_sbq = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const struct SLInterfaceID_ iid_android_cfg = { 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

/* The exported DATA symbols: `extern const SLInterfaceID SL_IID_X;` — dlsym
 * returns the address of these pointer variables, not of the GUIDs. */
static const SLInterfaceID SL_IID_NULL_v   = &iid_null;
static const SLInterfaceID SL_IID_OBJECT_v = &iid_object;
static const SLInterfaceID SL_IID_ENGINE_v = &iid_engine;
static const SLInterfaceID SL_IID_PLAY_v   = &iid_play;
static const SLInterfaceID SL_IID_RECORD_v = &iid_record;
static const SLInterfaceID SL_IID_VOLUME_v = &iid_volume;
static const SLInterfaceID SL_IID_BUFFERQUEUE_v = &iid_bufferqueue;
static const SLInterfaceID SL_IID_OUTPUTMIX_v   = &iid_outputmix;
static const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v = &iid_android_sbq;
static const SLInterfaceID SL_IID_ANDROIDCONFIGURATION_v     = &iid_android_cfg;

static int
iid_is(SLInterfaceID a, const struct SLInterfaceID_ *b)
{
    return a == b || (a != NULL && memcmp(a, b, sizeof(*b)) == 0);
}

static const char *
iid_name(SLInterfaceID iid)
{
    if (iid == NULL) return "(null)";
    if (iid_is(iid, &iid_engine)) return "ENGINE";
    if (iid_is(iid, &iid_play)) return "PLAY";
    if (iid_is(iid, &iid_record)) return "RECORD";
    if (iid_is(iid, &iid_volume)) return "VOLUME";
    if (iid_is(iid, &iid_bufferqueue)) return "BUFFERQUEUE";
    if (iid_is(iid, &iid_outputmix)) return "OUTPUTMIX";
    if (iid_is(iid, &iid_android_sbq)) return "ANDROIDSIMPLEBUFFERQUEUE";
    if (iid_is(iid, &iid_android_cfg)) return "ANDROIDCONFIGURATION";
    if (iid_is(iid, &iid_object)) return "OBJECT";
    if (iid_is(iid, &iid_null)) return "NULL";
    return "unknown";
}

/* ── objects ───────────────────────────────────────────────────────────────── */
enum sl_kind { SL_KIND_ENGINE, SL_KIND_OUTPUTMIX, SL_KIND_PLAYER };

struct sl_buf { const void *data; SLuint32 size; };

struct sl_object {
    /* One vtable pointer per interface; an interface handle is the ADDRESS of
     * its field, and container_of() gets back to the object. */
    const struct SLObjectItf_                   *itf_object;
    const struct SLEngineItf_                   *itf_engine;
    const struct SLPlayItf_                     *itf_play;
    const struct SLAndroidSimpleBufferQueueItf_ *itf_bq;
    const struct SLAndroidConfigurationItf_     *itf_cfg;
    const struct SLVolumeItf_                   *itf_volume;

    enum sl_kind kind;
    SLuint32     state;
    int          id;

    /* player */
    SLDataFormat_PCM fmt;
    SLuint32         capacity;
    struct sl_buf   *queue;
    SLuint32         q_head, q_count;
    int              playing_one;     /* a popped buffer is being written */
    SLuint32         play_state;
    SLAndroidSimpleBufferQueueCallback bq_cb;
    void            *bq_ctx;
    SLPlayCallback   play_cb;
    void            *play_ctx;
    SLmillibel       volume_mb;
    SLboolean        mute;

    AudioTrack      *track;
    pthread_t        thread;
    int              thread_started;
    int              quit;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;

    unsigned long long bytes_out;
    unsigned long      buffers_out;
    unsigned long      callbacks;
};

#define SL_FROM(ptr, field) \
    ((struct sl_object *)((char *)(ptr) - offsetof(struct sl_object, field)))

static int sl_object_ids = 0;

static void
sl_unsupported(const char *what)
{
    fprintf(stderr, "[OPENSL] %s -> FEATURE_UNSUPPORTED\n", what);
}

/* ── ObjectItf ─────────────────────────────────────────────────────────────── */
static void *player_thread(void *arg);

static SLresult SOFTFP
obj_Realize(SLObjectItf self, SLboolean async)
{
    struct sl_object *o = SL_FROM(self, itf_object);
    if (o->state == SL_OBJECT_STATE_REALIZED)
        return SL_RESULT_PRECONDITIONS_VIOLATED;

    if (o->kind == SL_KIND_PLAYER) {
        int rate = (int)(o->fmt.samplesPerSec / 1000);
        int ch = (int)o->fmt.numChannels;
        int bytes_per_frame = ch * (int)(o->fmt.bitsPerSample / 8);
        /* Hint the ring at ~2 of the app's buffers; the app's buffer size is
         * not known until the first Enqueue, so use ~46 ms of audio. */
        o->track = apkenv_audiotrack_create(rate, ch, (rate / 22) * bytes_per_frame);
        if (o->track == NULL) {
            fprintf(stderr, "[OPENSL] player#%d Realize: AudioTrack %d Hz/%d ch FAILED\n",
                    o->id, rate, ch);
            return SL_RESULT_RESOURCE_ERROR;
        }
        if (pthread_create(&o->thread, NULL, player_thread, o) != 0) {
            fprintf(stderr, "[OPENSL] player#%d Realize: pthread_create failed\n", o->id);
            apkenv_audiotrack_release(o->track);
            o->track = NULL;
            return SL_RESULT_RESOURCE_ERROR;
        }
        o->thread_started = 1;
    }
    o->state = SL_OBJECT_STATE_REALIZED;
    fprintf(stderr, "[OPENSL] %s#%d Realize(async=%u) -> ok\n",
            o->kind == SL_KIND_ENGINE ? "engine" : o->kind == SL_KIND_OUTPUTMIX ? "outputmix" : "player",
            o->id, async);
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
obj_Resume(SLObjectItf self, SLboolean async)
{
    struct sl_object *o = SL_FROM(self, itf_object);
    return o->state == SL_OBJECT_STATE_REALIZED ? SL_RESULT_SUCCESS
                                                : SL_RESULT_PRECONDITIONS_VIOLATED;
}

static SLresult SOFTFP
obj_GetState(SLObjectItf self, SLuint32 *pState)
{
    struct sl_object *o = SL_FROM(self, itf_object);
    if (pState == NULL) return SL_RESULT_PARAMETER_INVALID;
    *pState = o->state;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface)
{
    struct sl_object *o = SL_FROM(self, itf_object);
    void *itf = NULL;

    if (pInterface == NULL) return SL_RESULT_PARAMETER_INVALID;
    if (iid_is(iid, &iid_object))
        itf = &o->itf_object;
    else if (o->kind == SL_KIND_ENGINE && iid_is(iid, &iid_engine))
        itf = &o->itf_engine;
    else if (o->kind == SL_KIND_PLAYER && iid_is(iid, &iid_play))
        itf = &o->itf_play;
    else if (o->kind == SL_KIND_PLAYER &&
             (iid_is(iid, &iid_android_sbq) || iid_is(iid, &iid_bufferqueue)))
        itf = &o->itf_bq;
    else if (o->kind == SL_KIND_PLAYER && iid_is(iid, &iid_android_cfg))
        itf = &o->itf_cfg;
    else if (o->kind == SL_KIND_PLAYER && iid_is(iid, &iid_volume))
        itf = &o->itf_volume;

    /* The spec lets the configuration interface be fetched before Realize
     * (FMOD does exactly that); everything else needs a realized object. */
    if (itf != NULL && o->state != SL_OBJECT_STATE_REALIZED && itf != &o->itf_cfg) {
        *(void **)pInterface = NULL;
        return SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    *(void **)pInterface = itf;
    if (itf == NULL) {
        fprintf(stderr, "[OPENSL] object#%d GetInterface(%s) -> FEATURE_UNSUPPORTED\n",
                o->id, iid_name(iid));
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
obj_RegisterCallback(SLObjectItf self, SLObjectCallback callback, void *pContext)
{
    return SL_RESULT_SUCCESS;   /* only async Realize reports through it */
}

static void SOFTFP
obj_AbortAsyncOperation(SLObjectItf self)
{
}

static void SOFTFP
obj_Destroy(SLObjectItf self)
{
    struct sl_object *o = SL_FROM(self, itf_object);

    if (o->kind == SL_KIND_PLAYER) {
        pthread_mutex_lock(&o->lock);
        o->quit = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        /* A writer blocked on a full ring must be released before the join. */
        if (o->track) apkenv_audiotrack_stop(o->track);
        /* Destroy from inside our own buffer-queue callback cannot join itself;
         * the worker sees quit and exits on return from the callback. */
        if (o->thread_started) {
            if (pthread_equal(o->thread, pthread_self())) {
                fprintf(stderr, "[OPENSL] player#%d Destroy from its own callback — "
                        "leaking the player instead of self-joining\n", o->id);
                return;
            }
            pthread_join(o->thread, NULL);
        }
        if (o->track) apkenv_audiotrack_release(o->track);
        fprintf(stderr, "[OPENSL] player#%d Destroy (%lu buffers, %llu bytes, %lu callbacks)\n",
                o->id, o->buffers_out, o->bytes_out, o->callbacks);
        free(o->queue);
        pthread_mutex_destroy(&o->lock);
        pthread_cond_destroy(&o->cond);
    } else {
        fprintf(stderr, "[OPENSL] %s#%d Destroy\n",
                o->kind == SL_KIND_ENGINE ? "engine" : "outputmix", o->id);
    }
    memset(o, 0, sizeof(*o));
    free(o);
}

static SLresult SOFTFP
obj_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable)
{
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
obj_GetPriority(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable)
{
    if (pPriority) *pPriority = 0;
    if (pPreemptable) *pPreemptable = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
obj_SetLossOfControlInterfaces(SLObjectItf self, SLint16 numInterfaces,
                               SLInterfaceID *pInterfaceIDs, SLboolean enabled)
{
    return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_vtbl = {
    obj_Realize,                    /* 0  */
    obj_Resume,                     /* 1  */
    obj_GetState,                   /* 2  */
    obj_GetInterface,               /* 3  */
    obj_RegisterCallback,           /* 4  */
    obj_AbortAsyncOperation,        /* 5  */
    obj_Destroy,                    /* 6  */
    obj_SetPriority,                /* 7  */
    obj_GetPriority,                /* 8  */
    obj_SetLossOfControlInterfaces, /* 9  */
};

/* ── PlayItf ───────────────────────────────────────────────────────────────── */
static SLresult SOFTFP
play_SetPlayState(SLPlayItf self, SLuint32 state)
{
    struct sl_object *o = SL_FROM(self, itf_play);
    SLuint32 old;

    if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING)
        return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->lock);
    old = o->play_state;
    o->play_state = state;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    if (state == SL_PLAYSTATE_PLAYING) apkenv_audiotrack_play(o->track);
    else apkenv_audiotrack_pause(o->track);

    if (old != state)
        fprintf(stderr, "[OPENSL] player#%d SetPlayState %u -> %u\n", o->id, old, state);
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
play_GetPlayState(SLPlayItf self, SLuint32 *pState)
{
    struct sl_object *o = SL_FROM(self, itf_play);
    if (pState == NULL) return SL_RESULT_PARAMETER_INVALID;
    *pState = o->play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
play_GetDuration(SLPlayItf self, SLmillisecond *pMsec)
{
    if (pMsec == NULL) return SL_RESULT_PARAMETER_INVALID;
    *pMsec = 0xFFFFFFFF;   /* SL_TIME_UNKNOWN */
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
play_GetPosition(SLPlayItf self, SLmillisecond *pMsec)
{
    struct sl_object *o = SL_FROM(self, itf_play);
    unsigned long long bytes_per_sec;
    if (pMsec == NULL) return SL_RESULT_PARAMETER_INVALID;
    bytes_per_sec = (unsigned long long)(o->fmt.samplesPerSec / 1000) *
                    o->fmt.numChannels * (o->fmt.bitsPerSample / 8);
    *pMsec = bytes_per_sec ? (SLmillisecond)(o->bytes_out * 1000ULL / bytes_per_sec) : 0;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
play_RegisterCallback(SLPlayItf self, SLPlayCallback callback, void *pContext)
{
    struct sl_object *o = SL_FROM(self, itf_play);
    o->play_cb = callback;       /* stored; no position/marker events are raised */
    o->play_ctx = pContext;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP play_SetCallbackEventsMask(SLPlayItf self, SLuint32 f) { return SL_RESULT_SUCCESS; }
static SLresult SOFTFP play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *p) { if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP play_SetMarkerPosition(SLPlayItf self, SLmillisecond m) { sl_unsupported("Play.SetMarkerPosition"); return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult SOFTFP play_ClearMarkerPosition(SLPlayItf self) { return SL_RESULT_SUCCESS; }
static SLresult SOFTFP play_GetMarkerPosition(SLPlayItf self, SLmillisecond *p) { sl_unsupported("Play.GetMarkerPosition"); return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult SOFTFP play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond m) { return SL_RESULT_SUCCESS; }
static SLresult SOFTFP play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond *p) { if (p) *p = 1000; return SL_RESULT_SUCCESS; }

static const struct SLPlayItf_ play_vtbl = {
    play_SetPlayState,              /* 0  */
    play_GetPlayState,              /* 1  */
    play_GetDuration,               /* 2  */
    play_GetPosition,               /* 3  */
    play_RegisterCallback,          /* 4  */
    play_SetCallbackEventsMask,     /* 5  */
    play_GetCallbackEventsMask,     /* 6  */
    play_SetMarkerPosition,         /* 7  */
    play_ClearMarkerPosition,       /* 8  */
    play_GetMarkerPosition,         /* 9  */
    play_SetPositionUpdatePeriod,   /* 10 */
    play_GetPositionUpdatePeriod,   /* 11 */
};

/* ── AndroidSimpleBufferQueueItf ───────────────────────────────────────────── */
static SLresult SOFTFP
bq_Enqueue(SLAndroidSimpleBufferQueueItf self, const void *pBuffer, SLuint32 size)
{
    struct sl_object *o = SL_FROM(self, itf_bq);
    SLresult r = SL_RESULT_SUCCESS;

    if (pBuffer == NULL || size == 0) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->lock);
    if (o->q_count >= o->capacity) {
        r = SL_RESULT_BUFFER_INSUFFICIENT;
    } else {
        struct sl_buf *b = &o->queue[(o->q_head + o->q_count) % o->capacity];
        b->data = pBuffer;
        b->size = size;
        o->q_count++;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    if (r != SL_RESULT_SUCCESS) {
        static int warned = 0;
        if (warned++ < 5)
            fprintf(stderr, "[OPENSL] player#%d Enqueue: queue full (%u) -> BUFFER_INSUFFICIENT\n",
                    o->id, o->capacity);
    }
    return r;
}

static SLresult SOFTFP
bq_Clear(SLAndroidSimpleBufferQueueItf self)
{
    struct sl_object *o = SL_FROM(self, itf_bq);
    pthread_mutex_lock(&o->lock);
    o->q_head = 0;
    o->q_count = 0;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
bq_GetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState *pState)
{
    struct sl_object *o = SL_FROM(self, itf_bq);
    if (pState == NULL) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->lock);
    pState->count = o->q_count + (o->playing_one ? 1 : 0);
    pState->index = (SLuint32)o->buffers_out;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
bq_RegisterCallback(SLAndroidSimpleBufferQueueItf self,
                    SLAndroidSimpleBufferQueueCallback callback, void *pContext)
{
    struct sl_object *o = SL_FROM(self, itf_bq);
    pthread_mutex_lock(&o->lock);
    o->bq_cb = callback;
    o->bq_ctx = pContext;
    pthread_mutex_unlock(&o->lock);
    fprintf(stderr, "[OPENSL] player#%d BufferQueue.RegisterCallback(%p, ctx=%p)\n",
            o->id, (void *)callback, pContext);
    return SL_RESULT_SUCCESS;
}

static const struct SLAndroidSimpleBufferQueueItf_ bq_vtbl = {
    bq_Enqueue,                     /* 0 */
    bq_Clear,                       /* 1 */
    bq_GetState,                    /* 2 */
    bq_RegisterCallback,            /* 3 */
};

/* ── AndroidConfigurationItf ───────────────────────────────────────────────── */
static SLresult SOFTFP
cfg_SetConfiguration(SLAndroidConfigurationItf self, const SLchar *configKey,
                     const void *pConfigValue, SLuint32 valueSize)
{
    struct sl_object *o = SL_FROM(self, itf_cfg);
    SLuint32 v = 0;
    if (pConfigValue && valueSize >= sizeof(SLuint32))
        memcpy(&v, pConfigValue, sizeof(v));
    /* androidPlaybackStreamType (3 = STREAM_MUSIC) has no meaning on one
     * shared SDL device; accept it the way Android does. */
    fprintf(stderr, "[OPENSL] player#%d SetConfiguration(\"%s\", %u) -> ok (ignored)\n",
            o->id, configKey ? (const char *)configKey : "(null)", v);
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
cfg_GetConfiguration(SLAndroidConfigurationItf self, const SLchar *configKey,
                     SLuint32 *pValueSize, void *pConfigValue)
{
    sl_unsupported("AndroidConfiguration.GetConfiguration");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLAndroidConfigurationItf_ cfg_vtbl = {
    cfg_SetConfiguration,           /* 0 */
    cfg_GetConfiguration,           /* 1 */
};

/* ── VolumeItf (stored; the SDL mix is not attenuated) ─────────────────────── */
static SLresult SOFTFP vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) { SL_FROM(self, itf_volume)->volume_mb = level; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *p) { if (!p) return SL_RESULT_PARAMETER_INVALID; *p = SL_FROM(self, itf_volume)->volume_mb; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *p) { if (!p) return SL_RESULT_PARAMETER_INVALID; *p = 0; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_SetMute(SLVolumeItf self, SLboolean m) { SL_FROM(self, itf_volume)->mute = m; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_GetMute(SLVolumeItf self, SLboolean *p) { if (!p) return SL_RESULT_PARAMETER_INVALID; *p = SL_FROM(self, itf_volume)->mute; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_EnableStereoPosition(SLVolumeItf self, SLboolean e) { return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean *p) { if (p) *p = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_SetStereoPosition(SLVolumeItf self, SLpermille s) { return SL_RESULT_SUCCESS; }
static SLresult SOFTFP vol_GetStereoPosition(SLVolumeItf self, SLpermille *p) { if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const struct SLVolumeItf_ volume_vtbl = {
    vol_SetVolumeLevel,             /* 0 */
    vol_GetVolumeLevel,             /* 1 */
    vol_GetMaxVolumeLevel,          /* 2 */
    vol_SetMute,                    /* 3 */
    vol_GetMute,                    /* 4 */
    vol_EnableStereoPosition,       /* 5 */
    vol_IsEnabledStereoPosition,    /* 6 */
    vol_SetStereoPosition,          /* 7 */
    vol_GetStereoPosition,          /* 8 */
};

/* ── the player's worker ───────────────────────────────────────────────────── */
static void *
player_thread(void *arg)
{
    struct sl_object *o = arg;
    unsigned long next_report = 0, last_underrun = 0;
    unsigned long bytes_per_sec = (o->fmt.samplesPerSec / 1000) * o->fmt.numChannels *
                                  (o->fmt.bitsPerSample / 8);

    for (;;) {
        struct sl_buf b;
        SLAndroidSimpleBufferQueueCallback cb;
        void *ctx;

        pthread_mutex_lock(&o->lock);
        while (!o->quit && (o->play_state != SL_PLAYSTATE_PLAYING || o->q_count == 0))
            pthread_cond_wait(&o->cond, &o->lock);
        if (o->quit) {
            pthread_mutex_unlock(&o->lock);
            break;
        }
        b = o->queue[o->q_head];
        o->q_head = (o->q_head + 1) % o->capacity;
        o->q_count--;
        o->playing_one = 1;
        pthread_mutex_unlock(&o->lock);

        /* Blocking: this is what paces the app's refill to real time. */
        apkenv_audiotrack_write(o->track, b.data, (int)b.size);

        pthread_mutex_lock(&o->lock);
        o->playing_one = 0;
        o->bytes_out += b.size;
        o->buffers_out++;
        cb = o->bq_cb;
        ctx = o->bq_ctx;
        if (o->quit) {
            pthread_mutex_unlock(&o->lock);
            break;
        }
        pthread_mutex_unlock(&o->lock);

        /* Heartbeat every ~10 s of audio: bytes/s against the format's rate
         * says the refill keeps real time; a 0 underrun delta says the ring
         * never ran dry. */
        if (bytes_per_sec) {
            unsigned long secs = (unsigned long)(o->bytes_out / bytes_per_sec);
            if (o->buffers_out == 1 || secs >= next_report) {
                unsigned long u = apkenv_audiotrack_underrun_bytes(o->track);
                fprintf(stderr, "[OPENSL] player#%d %lu buffers, %llu bytes (%lus of audio), "
                        "underrun +%lu\n", o->id, o->buffers_out, o->bytes_out, secs,
                        u - last_underrun);
                last_underrun = u;
                next_report = secs + 10;
            }
        }

        if (cb) {
            o->callbacks++;
            cb((SLAndroidSimpleBufferQueueItf)&o->itf_bq, ctx);
        }
    }
    return NULL;
}


/* ── EngineItf ─────────────────────────────────────────────────────────────── */
static const struct SLEngineItf_ engine_vtbl;

static struct sl_object *
sl_object_new(enum sl_kind kind)
{
    struct sl_object *o = calloc(1, sizeof(*o));
    if (o == NULL) return NULL;
    o->itf_object = &object_vtbl;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->id = ++sl_object_ids;
    if (kind == SL_KIND_ENGINE)
        o->itf_engine = &engine_vtbl;
    return o;
}

static SLresult SOFTFP
eng_CreateLEDDevice(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
                    SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{ sl_unsupported("Engine.CreateLEDDevice"); if (pDevice) *pDevice = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_CreateVibraDevice(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
                      SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{ sl_unsupported("Engine.CreateVibraDevice"); if (pDevice) *pDevice = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pAudioSrc,
                      SLDataSink *pAudioSnk, SLuint32 numInterfaces,
                      const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired)
{
    SLDataLocator_BufferQueue *loc;
    SLDataFormat_PCM *fmt;
    SLDataLocator_OutputMix *sink;
    struct sl_object *o;
    SLuint32 i;

    if (pPlayer == NULL) return SL_RESULT_PARAMETER_INVALID;
    *pPlayer = NULL;
    if (pAudioSrc == NULL || pAudioSrc->pLocator == NULL || pAudioSrc->pFormat == NULL ||
        pAudioSnk == NULL || pAudioSnk->pLocator == NULL)
        return SL_RESULT_PARAMETER_INVALID;

    loc = pAudioSrc->pLocator;
    fmt = pAudioSrc->pFormat;
    sink = pAudioSnk->pLocator;
    if ((loc->locatorType != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE &&
         loc->locatorType != SL_DATALOCATOR_BUFFERQUEUE) ||
        fmt->formatType != SL_DATAFORMAT_PCM || sink->locatorType != SL_DATALOCATOR_OUTPUTMIX) {
        fprintf(stderr, "[OPENSL] CreateAudioPlayer: source locator 0x%x format %u sink 0x%x "
                "-> CONTENT_UNSUPPORTED (only PCM buffer queue -> output mix)\n",
                loc->locatorType, fmt->formatType, sink->locatorType);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    if (fmt->bitsPerSample != 16 || (fmt->numChannels != 1 && fmt->numChannels != 2) ||
        fmt->endianness == SL_BYTEORDER_BIGENDIAN || fmt->samplesPerSec < 1000) {
        fprintf(stderr, "[OPENSL] CreateAudioPlayer: PCM %u ch %u mHz %u bit endian %u "
                "-> CONTENT_UNSUPPORTED (S16LE mono/stereo only)\n", fmt->numChannels,
                fmt->samplesPerSec, fmt->bitsPerSample, fmt->endianness);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }

    for (i = 0; i < numInterfaces && pInterfaceIds; i++) {
        SLInterfaceID iid = pInterfaceIds[i];
        int known = iid_is(iid, &iid_play) || iid_is(iid, &iid_android_sbq) ||
                    iid_is(iid, &iid_bufferqueue) || iid_is(iid, &iid_android_cfg) ||
                    iid_is(iid, &iid_volume) || iid_is(iid, &iid_object);
        int required = pInterfaceRequired ? pInterfaceRequired[i] : 0;
        if (!known) {
            fprintf(stderr, "[OPENSL] CreateAudioPlayer: interface %s %s\n", iid_name(iid),
                    required ? "REQUIRED -> FEATURE_UNSUPPORTED" : "(optional, not provided)");
            if (required) return SL_RESULT_FEATURE_UNSUPPORTED;
        }
    }

    o = sl_object_new(SL_KIND_PLAYER);
    if (o == NULL) return SL_RESULT_MEMORY_FAILURE;
    o->itf_play = &play_vtbl;
    o->itf_bq = &bq_vtbl;
    o->itf_cfg = &cfg_vtbl;
    o->itf_volume = &volume_vtbl;
    o->fmt = *fmt;
    o->capacity = loc->numBuffers ? loc->numBuffers : 1;
    o->queue = calloc(o->capacity, sizeof(*o->queue));
    o->play_state = SL_PLAYSTATE_STOPPED;
    if (o->queue == NULL) { free(o); return SL_RESULT_MEMORY_FAILURE; }
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);

    fprintf(stderr, "[OPENSL] CreateAudioPlayer -> player#%d: PCM %u Hz, %u ch, %u bit, "
            "mask 0x%x, endian %u; queue of %u buffers; %u interfaces requested\n",
            o->id, fmt->samplesPerSec / 1000, fmt->numChannels, fmt->bitsPerSample,
            fmt->channelMask, fmt->endianness, o->capacity, numInterfaces);
    *pPlayer = (SLObjectItf)&o->itf_object;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
eng_CreateAudioRecorder(SLEngineItf self, SLObjectItf *pRecorder, SLDataSource *pAudioSrc,
                        SLDataSink *pAudioSnk, SLuint32 n, const SLInterfaceID *ids,
                        const SLboolean *req)
{ sl_unsupported("Engine.CreateAudioRecorder"); if (pRecorder) *pRecorder = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_CreateMidiPlayer(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pMIDISrc,
                     SLDataSource *pBankSrc, SLDataSink *pAudioOutput, SLDataSink *pVibra,
                     SLDataSink *pLEDArray, SLuint32 n, const SLInterfaceID *ids,
                     const SLboolean *req)
{ sl_unsupported("Engine.CreateMidiPlayer"); if (pPlayer) *pPlayer = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_CreateListener(SLEngineItf self, SLObjectItf *pListener, SLuint32 n,
                   const SLInterfaceID *ids, const SLboolean *req)
{ sl_unsupported("Engine.CreateListener"); if (pListener) *pListener = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_Create3DGroup(SLEngineItf self, SLObjectItf *pGroup, SLuint32 n,
                  const SLInterfaceID *ids, const SLboolean *req)
{ sl_unsupported("Engine.Create3DGroup"); if (pGroup) *pGroup = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces,
                    const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired)
{
    struct sl_object *o;
    SLuint32 i;
    if (pMix == NULL) return SL_RESULT_PARAMETER_INVALID;
    *pMix = NULL;
    for (i = 0; i < numInterfaces && pInterfaceIds; i++)
        if (pInterfaceRequired && pInterfaceRequired[i] &&
            !iid_is(pInterfaceIds[i], &iid_object)) {
            fprintf(stderr, "[OPENSL] CreateOutputMix: required interface %s -> "
                    "FEATURE_UNSUPPORTED\n", iid_name(pInterfaceIds[i]));
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
    o = sl_object_new(SL_KIND_OUTPUTMIX);
    if (o == NULL) return SL_RESULT_MEMORY_FAILURE;
    fprintf(stderr, "[OPENSL] CreateOutputMix -> outputmix#%d\n", o->id);
    *pMix = (SLObjectItf)&o->itf_object;
    return SL_RESULT_SUCCESS;
}

static SLresult SOFTFP
eng_CreateMetadataExtractor(SLEngineItf self, SLObjectItf *pObj, SLDataSource *pDataSource,
                            SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{ sl_unsupported("Engine.CreateMetadataExtractor"); if (pObj) *pObj = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_CreateExtensionObject(SLEngineItf self, SLObjectItf *pObj, void *pParameters,
                          SLuint32 objectID, SLuint32 n, const SLInterfaceID *ids,
                          const SLboolean *req)
{ sl_unsupported("Engine.CreateExtensionObject"); if (pObj) *pObj = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_QueryNumSupportedInterfaces(SLEngineItf self, SLuint32 objectID, SLuint32 *pNum)
{ if (pNum) *pNum = 0; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_QuerySupportedInterfaces(SLEngineItf self, SLuint32 objectID, SLuint32 index,
                             SLInterfaceID *pInterfaceId)
{ if (pInterfaceId) *pInterfaceId = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult SOFTFP
eng_QueryNumSupportedExtensions(SLEngineItf self, SLuint32 *pNumExtensions)
{ if (pNumExtensions) *pNumExtensions = 0; return SL_RESULT_SUCCESS; }

static SLresult SOFTFP
eng_QuerySupportedExtension(SLEngineItf self, SLuint32 index, SLchar *pExtensionName,
                            SLint16 *pNameLength)
{ return SL_RESULT_PARAMETER_INVALID; }

static SLresult SOFTFP
eng_IsExtensionSupported(SLEngineItf self, const SLchar *pExtensionName, SLboolean *pSupported)
{ if (pSupported) *pSupported = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }

static const struct SLEngineItf_ engine_vtbl = {
    eng_CreateLEDDevice,             /* 0  */
    eng_CreateVibraDevice,           /* 1  */
    eng_CreateAudioPlayer,           /* 2  (+8, checked in libfmodex) */
    eng_CreateAudioRecorder,         /* 3  */
    eng_CreateMidiPlayer,            /* 4  */
    eng_CreateListener,              /* 5  */
    eng_Create3DGroup,               /* 6  */
    eng_CreateOutputMix,             /* 7  (+28, checked in libfmodex) */
    eng_CreateMetadataExtractor,     /* 8  */
    eng_CreateExtensionObject,       /* 9  */
    eng_QueryNumSupportedInterfaces, /* 10 */
    eng_QuerySupportedInterfaces,    /* 11 */
    eng_QueryNumSupportedExtensions, /* 12 */
    eng_QuerySupportedExtension,     /* 13 */
    eng_IsExtensionSupported,        /* 14 */
};

/* ── entry point ───────────────────────────────────────────────────────────── */
static SLresult SOFTFP
slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const SLEngineOption *pEngineOptions,
               SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
               const SLboolean *pInterfaceRequired)
{
    struct sl_object *o;
    SLuint32 i;
    if (pEngine == NULL) return SL_RESULT_PARAMETER_INVALID;
    *pEngine = NULL;
    for (i = 0; i < numInterfaces && pInterfaceIds; i++)
        if (pInterfaceRequired && pInterfaceRequired[i] &&
            !iid_is(pInterfaceIds[i], &iid_engine) && !iid_is(pInterfaceIds[i], &iid_object)) {
            fprintf(stderr, "[OPENSL] slCreateEngine: required interface %s -> "
                    "FEATURE_UNSUPPORTED\n", iid_name(pInterfaceIds[i]));
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
    o = sl_object_new(SL_KIND_ENGINE);
    if (o == NULL) return SL_RESULT_MEMORY_FAILURE;
    fprintf(stderr, "[OPENSL] slCreateEngine -> engine#%d (%u options, %u interfaces)\n",
            o->id, numOptions, numInterfaces);
    *pEngine = (SLObjectItf)&o->itf_object;
    return SL_RESULT_SUCCESS;
}

/* ── the library's exports, and the opt-in ─────────────────────────────────── */
static int opensles_on = 0;

void apkenv_opensles_enable(void)
{
    if (!opensles_on)
        fprintf(stderr, "[OPENSL] libOpenSLES.so shim enabled\n");
    opensles_on = 1;
}

int apkenv_opensles_enabled(void)
{
    return opensles_on;
}

void *
apkenv_opensles_dlsym(const char *symbol)
{
    static const struct { const char *name; void *addr; } exports[] = {
        { "slCreateEngine",                  (void *)slCreateEngine },
        { "SL_IID_NULL",                     (void *)&SL_IID_NULL_v },
        { "SL_IID_OBJECT",                   (void *)&SL_IID_OBJECT_v },
        { "SL_IID_ENGINE",                   (void *)&SL_IID_ENGINE_v },
        { "SL_IID_PLAY",                     (void *)&SL_IID_PLAY_v },
        { "SL_IID_RECORD",                   (void *)&SL_IID_RECORD_v },
        { "SL_IID_VOLUME",                   (void *)&SL_IID_VOLUME_v },
        { "SL_IID_BUFFERQUEUE",              (void *)&SL_IID_BUFFERQUEUE_v },
        { "SL_IID_OUTPUTMIX",                (void *)&SL_IID_OUTPUTMIX_v },
        { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v },
        { "SL_IID_ANDROIDCONFIGURATION",     (void *)&SL_IID_ANDROIDCONFIGURATION_v },
    };
    size_t i;
    if (symbol == NULL) return NULL;
    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
        if (strcmp(symbol, exports[i].name) == 0)
            return exports[i].addr;
    fprintf(stderr, "[OPENSL] dlsym(libOpenSLES.so, \"%s\") -> not provided\n", symbol);
    return NULL;
}
