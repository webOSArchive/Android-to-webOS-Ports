---
name: call-the-engines-jni-onload
description: "An apkenv module must call the game engine's own JNI_OnLoad — it is where the engine RegisterNatives its callbacks, and skipping it fails silently and far from the cause"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b8d186fb-77d5-48ec-aeb3-61d9b159a425
  modified: 2026-09-15T14:43:47.300Z
---

When writing an apkenv support module, check `nm -D <engine>.so | grep JNI_OnLoad` and **call it
first thing in `init()`**, resolving it from the *game* lib by name
(`LOOKUP_LIBM("lib<game>", "JNI_OnLoad")`) — apks often ship a second `.so` with one of its own.

**Why:** Dalvik runs `JNI_OnLoad` at `System.loadLibrary` time, and that is where an engine
`RegisterNatives` **its own callbacks** — every Java method the smali declares `native`. Skip it and
they are all unbound, with no error. In Fruit Ninja (Halfbrick Mortar, 2026-09-15) one missing
`JNI_OnLoad` produced three symptoms that looked unrelated: no audio at all, a worker thread that
exited the instant it called `native_threadEntry`, and dialogs that could never have been answered.
32 callbacks registered the moment it was called, and sound started in the same run.

**How to apply:** add it to the contract pass, not the debugging pass. Split the contract table by
modifier — rows declared `native` are engine **callbacks**, answered by calling the engine's own
registered function (`jnienv_find_native_method(class, name)`), never by inventing a return value.
And when one mechanism is unbound, expect several unrelated-looking symptoms from it: check for a
single cause before theorising separately about audio and threading.

Related: [[static-answer-before-dynamic-probe]] (the static pass said `native_threadEntry` was dead
code; the always-on tracer overruled it on the first device run),
[[systematic-not-brute-force]], [[wrapper-spike-progress]].
