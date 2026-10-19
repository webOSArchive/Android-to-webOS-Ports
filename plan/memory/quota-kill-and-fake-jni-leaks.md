---
name: quota-kill-and-fake-jni-leaks
description: "A webOS \"crash\" with no signal in the log is usually LunaSysMgr's requiredMemory quota kill on focus loss; apkenv's fake JNI never frees, so per-frame NewXArray/NewStringUTF leak"
metadata:
  node_type: memory
  type: feedback
  originSessionId: b20a594d-588d-45ab-9d63-ac12e0ed4974
  modified: 2026-10-17T17:36:57.097Z
---

When a packaged port "crashes" and the apkenv log just stops (often right after `ACTIVEEVENT gain=0`) with no signal line, grep the device's `/var/log/messages` for `MemoryMonitor … exceeded its memory quota. ProcMem = N, restriction = M`. Once the process passes appinfo `requiredMemory`, LunaSysMgr SIGKILLs it at the next focus loss (home button, notification, dashboard). PvZ HD 1.0.1 (2026-10-17): "crashes at level 16" was this, after a long session.

Second half: apkenv's fake JNI (`jni/jnienv.c`) never frees anything. `DeleteLocalRef` and `Release*ArrayElements` do nothing, and `NewStringUTF`/`NewXArray`/`GetMethodID` all malloc. Any per-frame or per-audio-callback JNI allocation in a module is a leak. PvZ's audio pump leaked ~9 MB/min from `NewShortArray` per callback.

**Why:** the user reads "crash at level N" as a level/graphics bug; the real cause was memory growth over playtime plus a quota set below the steady state.

**How to apply:** for any new module, measure RSS at idle over a few minutes (marmalade.c `[MEM]` heartbeat is the template), set `requiredMemory` above the measured peak, and allocate JNI arrays/strings once in hot paths. Related: [[probe-every-path-the-engine-takes]], [[dont-touch-shipped-ports]].
