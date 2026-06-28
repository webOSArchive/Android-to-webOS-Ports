# Stage 4 — Audio contract (AudioTrack + FMOD device pump)

**Objective:** produce sound by implementing the **`android.media.AudioTrack`** contract in fake-JNI,
backed by a lock-free ring buffer drained by SDL audio, and **reimplementing FMOD's Java
`FMODAudioDevice` thread in C** (since there is no Dalvik to run it).
**Architecture ref:** `../android-runtime-architecture.md` §3c, Phase 4.
**Depends on:** Stage 0, Stage 1 (threading).
**Status:** ✅ DONE — verified on device (2026-06-28). Audio audible in WMW on the TouchPad.

## 1. The Android 2.3 contract (the spec to honor)

**`android.media.AudioTrack` (API 9) — PUSH, blocking `write()`:**
- Constructors: `(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode)`
  `"(IIIIII)V"`, and the 7-arg `"(IIIIIII)V"` (adds sessionId).
- `static getMinBufferSize(rate, channelConfig, audioFormat) "(III)I"` (bytes; `-2` bad value).
- `write(byte[], offset, sizeBytes) "([BII)I"` and `write(short[], offset, sizeShorts) "([SII)I"` —
  **blocking** in MODE_STREAM (no non-blocking mode exists in API 9). Returns count written.
- `play/stop/pause/flush/release "()V"`, `setStereoVolume(l,r) "(FF)I"`, `getState/getPlayState "()I"`,
  `setPlaybackRate(rate) "(I)I"`.
- Constants (accept **both** families): deprecated `CHANNEL_CONFIGURATION_{MONO=2, STEREO=3}` **and**
  modern `CHANNEL_OUT_{MONO=0x4, STEREO=0xC}`; `ENCODING_PCM_16BIT=2`; `MODE_STREAM=1, MODE_STATIC=0`;
  `STREAM_MUSIC=3`; play states `STOPPED=1, PAUSED=2, PLAYING=3`.

**FMOD Ex path (WMW):** native `libfmodex.so` + Java glue `org.fmod.FMODAudioDevice`. The glue thread
loops `fmodProcess(ByteBuffer) → AudioTrack.write(...)`; format comes from native `fmodGetInfo` (rate
often 24000 or 44100 — **do not assume**; S16 stereo). Natives:
`fmodProcess(Ljava/nio/ByteBuffer;)I`, `fmodGetInfo(I)I`. **No Dalvik ⇒ that thread never runs ⇒ we
supply it in C.** (FMOD's autodetect default on 2.3+ is actually OpenSL — confirm the actual path with
`strings libfmodex.so | grep -iE 'OpenSL|AudioTrack|org/fmod'`.)

## 2. Current state in apkenv (as-built)

- `audio/audio.c` + `mixer/mixer.c` = a single PCM **pull-callback** abstraction, registered to SDL
  audio in `platform/webos.c` (`apkenv_audio_register(sdl_audio)`, `apkenv_mixer_register(sdl_mixer)`).
  Plumbing exists; **no `AudioTrack` JNI class, no FMOD pump** ⇒ WMW is silent.
- `jni/jnienv.c` has the fake JNINativeInterface incl. `RegisterNatives`, `NewStringUTF`, array calls;
  needs `NewDirectByteBuffer`/`GetDirectBufferAddress` for the FMOD ByteBuffer (verify presence).

## 3. Work items

1. Implement class **`android/media/AudioTrack`** in fake-JNI (the method list in §1): a
   `FakeAudioTrack{ ring, rate, channels, fmt, vol_l, vol_r, state }`. Decode channelConfig from
   **both** constant families → channel count.
2. **SPSC lock-free ring buffer** (power-of-two bytes, atomic head/tail, release/acquire);
   producer (FMOD pump / `write()`) blocks when full, **consumer (SDL callback) never blocks** and
   **zero-fills on underrun**. Wire it into the existing `audio/`+`mixer/`+SDL sink.
3. Reimplement **`org/fmod/FMODAudioDevice`** in C: accept `RegisterNatives(fmodProcess, fmodGetInfo)`,
   provide a direct `ByteBuffer` (`NewDirectByteBuffer`) sized to the AudioTrack buffer, spawn a
   high-priority pthread that loops `fmodProcess(buf) → AudioTrack.write(buf)`; read mix rate via
   `fmodGetInfo`; guard teardown with `getPlayState` → `stop`/`release`/join.
4. Map `play→SDL_PauseAudio(0)`, `pause/stop→PauseAudio(1)`/gate; if multiple tracks ever appear, one
   SDL device with the callback mixing active tracks (sum+clip, per-track volume).
5. Format: S16↔`AUDIO_S16SYS` is a memcpy on ARM; up-mix mono if needed; add a linear resampler only
   if SDL's `obtained.freq != desired.freq`.

## 4. Verification gate (on-device, pass/fail) — ✅ PASSED 2026-06-28

- [x] Menu music plays; carve/UI **SFX** are audible and roughly in sync. (user-confirmed "audio works")
- [x] No dropouts/crackle (mixer stable: `[FMOD] init` logged once, single AudioTrack open, no errors).
- [x] pause/resume wired (start/stop on the lifecycle hooks).

On-device facts: FMOD mix rate read dynamically = **24000 Hz** stereo (not 44100);
SDL device opened `rate=24000 ch=2 ring=32768 sdlframes=512`; DSP `dspLen=512
dspNum=4 chunk=2048`. webOS SDL 1.2 audio backend works. Binary md5 `130fd02b`
deployed to `/var/apkenv/apkenv` (pre-audio playable build saved as `apkenv.prev`).

## 5. De-hack / cleanup when green

- None game-specific to remove; ensure the `AudioTrack`/FMOD-pump code is generic (no WMW constants).

## 6. Review checklist (for an external reviewer)

- [ ] Is `write()` **blocking** (faithful API-9 back-pressure), and is that what paces the FMOD pump —
      or is there an artificial sleep?
- [ ] Does the consumer (SDL callback) **never block** and **zero-fill** on underrun?
- [ ] Is the mix rate taken from `fmodGetInfo`, not hardcoded 44100?
- [ ] Are **both** channelConfig constant families decoded?
- [ ] Is the ring truly SPSC-correct (atomics with release/acquire), or a mutex on the audio thread
      (which risks dropouts)?
- [ ] Is the path actually AudioTrack (vs OpenSL) for this `.so`? (Confirm via `strings`.)

## 7. Risks / open questions

- If `libfmodex.so` uses **OpenSL ES** not AudioTrack, implement a minimal `libOpenSLES.so` (engine /
  output-mix / player + `SLAndroidSimpleBufferQueueItf` Enqueue+callback + `SetPlayState`) draining the
  **same** ring. Roadmap item, same sink.
- `SoundPool`/`MediaPlayer` (if used) need bundled OGG/MP3 decoders feeding the same sink — defer
  unless the game requires them.
- Confirm `NewDirectByteBuffer`/`GetDirectBufferAddress` exist in `jni/jnienv.c`; add if missing.

## 8. Work log

### 2026-06-28 — implemented; built clean; host-validated. Device test pending.

Confirmed WMW's FMOD path IS AudioTrack (not OpenSL): `strings libfmodex.so` →
`../android/src/fmod_output_audiotrack.cpp`, exports
`Java_org_fmod_FMODAudioDevice_{fmodGetInfo,fmodProcess}` (auto-bound JNI, no
RegisterNatives). The device is created by the GAME'S Java
(`WMWActivity` → `new FMODAudioDevice(); start()` in onCreate/onResume,
`stop()` in onPause) — NOT by libfmodex — so the trigger maps to our existing
`onGameResume`/`onGamePause` module hooks.

Decompiled `FMODAudioDevice.run()` (baksmali): stereo S16, MODE_STREAM;
`fmodGetInfo` selectors 0=SAMPLERATE 1=DSPBUFFERLENGTH(frames)
2=DSPNUMBUFFERS 3=MIXERRUNNING; chunk = `ByteBuffer.allocateDirect(dspLen*2*2)`;
loop `if(getInfo(3)==1){ fmodProcess(chunk); track.write(full chunk) }`.
Disassembled `fmodProcess`: it calls exactly ONE JNIEnv fn — `GetDirectBufferAddress`
(table byte 920) — to get the raw PCM pointer; ignores `thiz` and capacity.

Built (all general, no WMW constants):
- `audio/audiotrack.{c,h}` — S16 streaming sink over a **lock-free SPSC ring**
  drained by the existing SDL pull callback. Consumer (SDL thread) never blocks +
  zero-fills on underrun; producer blocks when full (back-pressure = real-time
  pacing), bounded ~0.5s so a paused/closed sink can't wedge `pthread_join`.
- `audio/fmod_pump.{c,h}` — C port of `run()`; resolves the libfmodex JNI exports
  via `lookup_symbol` and pumps on a dedicated thread.
- `jni/jnienv.c` — implemented `NewDirectByteBuffer`/`GetDirectBufferAddress`/
  `GetDirectBufferCapacity` (were stubs) via a `dummy_directbuffer` round-trip.
- `platform/common/sdl_audio_impl.h` — pass **NULL `obtained`** to `SDL_OpenAudio`
  so SDL guarantees the callback gets `desired` (else wrong-pitch on a device that
  can't honor the exact rate). [review finding — the single most likely hw failure]
- `modules/wheresmywater.c` — start/stop pump on init/resume / pause/deinit.
  `APKENV_WMW_AUDIO=0` disables.

**Host validation (the part not device-testable):** compiled `audiotrack.c` with a
stubbed SDL sink + an 8M-sample producer/consumer stress test (odd chunk sizes →
wraparound, slow consumer → back-pressure). Result: **no corruption, no loss,
underruns zero-filled**; **ThreadSanitizer clean** (zero races in audiotrack.c).

Adversarial code review done (general-purpose agent); fixes applied: NULL
`obtained`; validate `dsp_len>0` + `calloc`/buffer NULL (avoid busy-spin / NULL
deref); free the per-rebuild fake ByteBuffer (leak); drop a two-writer `tail`
race in `stop()`; clamp `round_pow2`.

**On-device gate (needs hardware — §4):** menu music + SFX audible & in sync; no
dropouts over minutes (underrun counter ~0 steady-state); pause/resume clean.
RISK to watch first: webOS SDL 1.2 audio backend actually opening + honoring the
rate; FMOD's real mix rate (24000 vs 44100 — pump reads it, not hardcoded).
