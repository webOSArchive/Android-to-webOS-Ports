---
name: static-answer-before-dynamic-probe
description: Try to settle a question by reading the binary before instrumenting the running program — and check you are reading the RIGHT binary
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 318ea7b4-ab79-4275-8e97-a5c62dfe2908
  modified: 2026-08-27T22:16:27.004Z
---

When a question is "does this code ever do X?", spend the first ten minutes trying to answer it
**statically** — `strings`, `grep`, the decompiled Java, the settings file — before building or
trusting a runtime probe.

Temple Run 2's splash (2026-08-27) is the clean example. A previous session probed at runtime,
concluded "`splash.png` **is read**", and burned a device cycle forcing extra presents to reveal a
draw. Three greps settled it: the real libunity contains no `splash` string but its own
`UnitySplash*.png` watermarks, and the Java host has only the `splash_mode` settings key. **The
engine never opens that file**, so there was never a draw to reveal.

**Why:** a dynamic probe answers the question you actually instrumented, which is often not the
question you asked. This project has now recorded four probes that measured the wrong thing —
"FMOD never creates a stream thread" (apkenv's `pthread_create` hook only sees threads created
through it; `/proc/self/task` showed them), "the music data is read once" (a seek probe cannot see
sequential reads), "0 draws" (only one of two wrapper tables was instrumented), and this one. A
static fact has no such failure mode.

**How to apply:**
- **Check you are reading the real artifact first.** `lib/armeabi-v7a/libunity.so` in that apk is a
  43 KB **proxy**; the 6.8 MB engine is inside `assets/libs/armeabi-v7a/`. Grepping the proxy
  returns nothing and reads exactly like a finding. Sanity-check file size against the job the file
  supposedly does.
- Before believing a probe, ask **what else would produce this same reading**. The seek-range probe
  could not distinguish "opened splash.png" from "a read crossed the apk offset range splash.png
  occupies" — and a probe that cannot distinguish A from B will report A.
- If a probe must be dynamic, run a **positive control** that proves the probe works
  (`MONO_VERBOSE_METHOD=Awake` before trusting `=StartMainMenuMusic`), and prefer counting from
  something you do not control (`/proc/self/task`) over your own instrumentation.
- Absence of evidence in a *release* build is weak on its own: those "Creating OpenGLES1.x/2.0
  graphics device" strings exist with no code references at all. Confirm from a second angle.

Related: [[systematic-not-brute-force]], [[shim-owns-the-host-platforms-jobs]],
[[templerun2-port-analysis]].
