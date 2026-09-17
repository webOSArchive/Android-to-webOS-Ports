---
name: probe-every-path-the-engine-takes
description: "A tracer that covers only some syscalls produces confident false negatives — check which call the engine would actually use before believing 'it never asks for X'"
metadata:
  type: feedback
---

Before concluding "the engine never does X" from an instrument, check that the instrument covers
every call it could do X *with*. Read the engine's undefined-symbol list (`nm -D --undefined-only`)
and confirm the tracer hooks those specific functions.

**Why:** Dead Space's content path was wrong, and `APKENV_TRACE_FILES` — which traced only
`fopen`/`open` — reported "the engine opens no content file at all". True, and completely
misleading: the engine probes with `stat`/`opendir` first, so it was looking, failing, and never
reaching `open()`. That false negative was stated as established fact and sent a whole device run
into a GL theory that had no basis. Tracing `stat`/`opendir` gave the real answer in one run.

**How to apply:** when a probe returns a *negative*, treat it as the weakest kind of evidence and
verify the probe first (playbook: "always run a positive control"). Related trap in the same
session: apkenv's crash dump `on stack 0x...` lines are a **stack scan**, not a backtrace — they
list anything on the stack resembling a code address, so reading GL frames there as "it is in GL
now" invents a mechanism. `pc` is real, `lr` may be garbage.

Related: [[static-answer-before-dynamic-probe]], [[systematic-not-brute-force]],
[[call-the-engines-jni-onload]], [[wrapper-spike-progress]].
