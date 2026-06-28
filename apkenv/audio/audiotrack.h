/**
 * apkenv — android.media.AudioTrack contract (streaming PCM sink).
 *
 * A faithful re-implementation of the Android 2.3 AudioTrack MODE_STREAM
 * back-pressure model on top of apkenv's existing SDL audio backend
 * (audio/audio.h). PCM written by the producer (e.g. the FMOD device pump,
 * audio/fmod_pump.c, or any game's own audio thread) is queued in a
 * single-producer / single-consumer lock-free ring buffer; SDL's audio
 * callback drains it. The consumer never blocks and zero-fills on underrun;
 * the producer blocks when the ring is full (this is what paces the producer
 * to real time, exactly like AudioTrack.write() blocking on the hardware).
 *
 * 16-bit signed PCM only (the format every AudioTrack game here uses); add
 * up-mix / resample here if a game needs it.
 */
#ifndef APKENV_AUDIOTRACK_H
#define APKENV_AUDIOTRACK_H

typedef struct AudioTrack AudioTrack;

/* Create a streaming PCM sink: rate Hz, channels (1 or 2), S16.
 * hint_bytes is the engine's preferred audio buffer size; the ring is sized
 * to a small multiple of it so write() back-pressure tracks real time.
 * Lazily (re)opens the shared SDL audio device. Returns NULL on failure. */
AudioTrack *apkenv_audiotrack_create(int rate, int channels, int hint_bytes);

/* Blocking write of S16 PCM (faithful AudioTrack.write back-pressure).
 * Blocks while the ring is full; returns bytes accepted (== bytes unless the
 * track was stopped mid-write). */
int  apkenv_audiotrack_write(AudioTrack *t, const void *data, int bytes);

void apkenv_audiotrack_play(AudioTrack *t);    /* SDL_PauseAudio(0) */
void apkenv_audiotrack_pause(AudioTrack *t);   /* SDL_PauseAudio(1) */
void apkenv_audiotrack_stop(AudioTrack *t);    /* pause + flush + unblock writer */
void apkenv_audiotrack_release(AudioTrack *t); /* stop + free */

/* Diagnostics (Stage-0 underrun counter): total zero-filled bytes. */
unsigned long apkenv_audiotrack_underrun_bytes(AudioTrack *t);

#endif /* APKENV_AUDIOTRACK_H */
