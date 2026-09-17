---
name: shim-owns-the-host-platforms-jobs
description: "In a shim/emulation port, work the original PLATFORM did (not the engine) is missing by definition — do it yourself instead of hunting an engine path"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 318ea7b4-ab79-4275-8e97-a5c62dfe2908
  modified: 2026-08-27T22:16:42.276Z
---

When a port replaces a platform, ask of every missing behaviour: **did the engine do this, or did
the platform?** If it was the platform's job, there is no engine path to fix, and hunting one is
wasted effort. Do it in the shim.

Temple Run 2, 2026-08-27, both remaining gaps turned out to be this:

- **The boot splash.** On Android the Activity's window covers the load. apkenv has no Activity, so
  nothing covered it — the panel just held the last swapped buffer, black. The engine never opens
  `splash.png` at all. Fix: the host draws it, riding the present quad that already existed, and
  retires it on the engine's first draw call. ~100 lines.
- **The music volume slider.** The music bed is mixed in *underneath* the engine, so nothing in the
  engine could scale it. But a Unity game stores that setting in PlayerPrefs and **apkenv implements
  PlayerPrefs** — so one `novacom get` of the device's `playerprefs.txt` named the key and a
  two-line hook on `SetFloat` drove the mixer live.

**Why:** these read as engine bugs and invite disassembly, which is expensive and finds nothing.
Framed as "whose job was this?", both were a few hours including device confirmation, after earlier
sessions had burned runs on the engine-archaeology framing.

**How to apply:**
- List what the original platform provided that the shim does not: the window/Activity, the settings
  UI, the launcher, lifecycle callbacks, the audio device, permission dialogs. Anything missing from
  that list is yours to supply, not the engine's to be coaxed into.
- **Hosting a subsystem means you can READ it.** Before reverse-engineering a value out of an
  engine, look in the stores you already host — prefs, save files, the data dir. Cross-check against
  the game's own save file to be sure it is the setting and not an internal scalar.
- Prefer **self-timing handovers** over tuned durations: show the splash while the engine's draw
  count is zero rather than for N seconds, and a slower device simply holds it longer.
- Decode assets at **package time** when the shim links no decoder (raw PCM, raw RGB in an `EXTRAS`
  payload) — no runtime dependency, pre-sized for the target.

Related: [[static-answer-before-dynamic-probe]], [[templerun2-port-analysis]],
[[android-runtime-plan]].
