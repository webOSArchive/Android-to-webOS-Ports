/**
 * apkenv — OpenSL ES 1.0.1 (+ Android extensions) shim, "libOpenSLES.so".
 *
 * webOS has no OpenSL ES. Engines that can only output through it (Tiny Death
 * Star's libfmodex: its output autodetect dlopen()s libOpenSLES.so and uses
 * the buffer-queue player it finds) get this subset instead: an engine object,
 * an output mix, and a PCM buffer-queue audio player whose buffers are played
 * through audio/audiotrack.c. Recording is not offered.
 *
 * OFF by default. The library only exists for a module that opts in, so every
 * other port keeps getting "no libOpenSLES.so" from dlopen() exactly as before
 * (FMOD then picks its AudioTrack output, Mortar its Java mixer, ...).
 */
#ifndef APKENV_COMPAT_OPENSLES_H
#define APKENV_COMPAT_OPENSLES_H

/* Make "libOpenSLES.so" loadable (dlopen by basename, dlsym of slCreateEngine
 * and the SL_IID_* data symbols). Call from the module's try_init, before the
 * engine can probe for it. Idempotent. */
void apkenv_opensles_enable(void);
int  apkenv_opensles_enabled(void);

/* dlsym() on the builtin libOpenSLES.so handle: function or data address, or
 * NULL. Used by compat/hooks.c. */
void *apkenv_opensles_dlsym(const char *symbol);

#endif /* APKENV_COMPAT_OPENSLES_H */
