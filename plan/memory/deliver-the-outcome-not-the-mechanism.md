---
name: deliver-the-outcome-not-the-mechanism
description: "When an engine subsystem is stuck and unsafe to patch, deliver what the player perceives through a path that already works — using the game's own asset, gated per-game, matched to that path's format"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 318ea7b4-ab79-4275-8e97-a5c62dfe2908
  modified: 2026-08-27T22:18:46.577Z
---

When a subsystem inside the engine will not work and patching it is unsafe, stop asking "how do I
fix this code?" and ask **"what does the player need to perceive, and what path do I already have
that works?"**

Temple Run 2's music (2026-08-27) is how it shipped. Unity's native FMOD stream creates itself,
primes exactly one 64 KB buffer, and its channel never consumes — across several sessions,
everything else was ruled out (our pump, file IO, volume, the game logic, `fmodInitJni`, missing
threads). Inline-hooking libunity's FMOD internals to force it **crashed the device**, so the
mechanism was both unfixed and unsafe to keep poking. Meanwhile the AudioTrack pump was *already
proven* — SFX came through it fine. So: decode the game's own music track and mix it into that
known-good pump, after `fmodProcess()` and before the ring write. The player hears the game's real
music. The stream is still broken and documented as such.

**Why:** the port was one subsystem from complete, and the cost of continuing was device crashes
plus sessions with nothing the user could hear. "The mechanism is unfixable" and "the outcome is
undeliverable" are different claims, and only the first one was true.

**How to apply — what separates this from a fake:**
- **Use the game's own asset**, never a substitute. It is the real music, just carried differently.
- **Match the working path's format exactly.** The pump mixes into FMOD's own chunk buffer with no
  resampling, so the PCM had to be 24000 Hz stereo S16 — FMOD's *measured* output rate, not a
  guess. A workaround that fights its host path's format is just a new bug.
- **Gate it per-game with an env var** (`APKENV_FMOD_MUSIC_PCM`) so nothing else changes behaviour,
  and it disables itself on any read/alloc failure rather than degrading the real audio.
- **Wire it to the real controls** so it behaves like the thing it stands in for — the bed follows
  the in-game music slider via PlayerPrefs. See [[shim-owns-the-host-platforms-jobs]].
- **Mind the seams the real path handles for you:** ramp gain across the buffer (a step change at a
  chunk boundary is an audible click), and keep reading at zero volume so it stays in sync with
  wall-clock time.
- **State the limits in the docs, unprompted.** This bed loops and does not follow Unity's per-scene
  music source. An undocumented workaround becomes someone's mystery bug later.

**Asset traps worth expecting:** `sharedassets0.assets.resS` was **two concatenated MP3s with no
container** — Unity keeps only offsets, elsewhere — so decoding the file whole silently yields both
tracks back to back; split at `0xe4800` first. And when the shim links no decoder, decode at
**package time** into an `EXTRAS=` payload (raw PCM, raw RGB) with a script that checksums the
result: no runtime dependency, pre-sized for the target. See [[inheriting-in-flight-work]] for why
that script is not optional.

Related: [[templerun2-port-analysis]], [[static-answer-before-dynamic-probe]],
[[systematic-not-brute-force]].
