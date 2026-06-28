/**
 * apkenv — FMOD Ex "Audio Track Output" device pump.
 *
 * WMW (and other FMOD-Android games) ship libfmodex.so whose native mixer is
 * driven OUT to android.media.AudioTrack by a Java thread,
 * org.fmod.FMODAudioDevice.run(): it polls native fmodGetInfo() for the mix
 * format, builds an AudioTrack, then loops fmodProcess(ByteBuffer) ->
 * AudioTrack.write(). With no Dalvik that Java thread never runs, so we
 * re-implement run() in C here, calling the libfmodex JNI exports directly
 * (Java_org_fmod_FMODAudioDevice_fmodGetInfo / _fmodProcess) and feeding the
 * PCM into apkenv's AudioTrack sink (audio/audiotrack.h).
 *
 * Decoded from baksmali of FMODAudioDevice.run() (stereo S16, MODE_STREAM):
 *   fmodGetInfo selectors: 0=SAMPLERATE 1=DSPBUFFERLENGTH 2=DSPNUMBUFFERS
 *                          3=MIXERRUNNING ; NUMCHANNELS=2.
 *
 * Lifecycle mirrors WMWActivity: start() on create/resume, stop() on pause.
 */
#ifndef APKENV_FMOD_PUMP_H
#define APKENV_FMOD_PUMP_H

struct GlobalState;

/* Resolve the libfmodex JNI exports and spawn the pump thread. No-op (and
 * harmless) if the game has no FMOD device. Idempotent. */
void apkenv_fmod_pump_start(struct GlobalState *global);

/* Stop and join the pump thread. Idempotent. */
void apkenv_fmod_pump_stop(void);

#endif /* APKENV_FMOD_PUMP_H */
