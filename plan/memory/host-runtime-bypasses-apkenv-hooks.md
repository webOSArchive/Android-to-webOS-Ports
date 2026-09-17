---
name: host-runtime-bypasses-apkenv-hooks
description: "With a host (glibc) Mono, managed file I/O and dlopen bypass apkenv's hooks/tracers; and an \"offline\"/\"slow internet\" failure can be a missing LOCAL transport (Unity's Java WWW)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 56cd1e45-a09b-42c7-9921-09da8c3fcbf5
  modified: 2026-09-17T14:31:16.961Z
---

RoboCop (2026-09-17) showed Glu's "SLOW INTERNET CONNECTION" screen, while the same game boots offline under ACL. Two traps cost runs:

1. `APKENV_TRACE_FILES` showed no managed file access at all. The host-built Mono calls glibc directly, so apkenv's bionic hooks never see it. `strace -f -p <pid>` on the device (`/usr/bin/strace` exists) showed the real probes. P/Invoke is blind the same way: Mono's `dlopen` can't load a bionic `.so`, so a Mono dl fallback that goes through apkenv's linker is needed.
2. The cause wasn't the network at all. On Android, Unity's `WWW` is Java (`nativeInitWWW` + `com.unity3d.player.WWW`), and apkenv had none. So `jar:file://…obb!/assets/ABTesting.xml` never completed.

**Why:** a negative from a tracer that can't see the call path looks like evidence ([[probe-every-path-the-engine-takes]]). An error message's wording ("internet") points at the wrong layer.

**How to apply:** when a host runtime is bridged, probe with `strace`, not the apkenv tracers. When a game reports network trouble offline but works offline on a real device, list every transport the platform provided, including local ones like `file://`/`jar:` through WWW, before chasing network answers. Related: [[the-shim-owns-the-host-platforms-jobs]], [[static-answer-before-dynamic-probe]].
