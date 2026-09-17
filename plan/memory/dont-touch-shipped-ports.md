---
name: dont-touch-shipped-ports
description: "Shipped ports (e.g. Temple Run 2 1.4.0) are frozen — \"if it ain't broke, don't fix it\"; prefer systemic fixes that help future games, gated so shipped ports are untouched"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 61d92ecd-d145-4342-9fa7-c94c50c1fc83
  modified: 2026-09-14T14:10:14.220Z
---

Do not modify, rebuild, reinstall or "improve" a shipped port (Temple Run 2 1.4.0, PvZ HD, Amazing Alex,
WMW) while working on another game — the user said (2026-09-14, during Aralon): "don't touch temple run 2 --
if it ain't broke, don't fix it." TR2's music works for the player (host-mixed PCM bed); that its native
FMOD stream never worked is not a reason to reopen it.

**Why:** shipped packages are known-good on the device; any shared-code change risks a regression the user
then has to find.

**How to apply:** gate new behaviour in shared modules (e.g. `modules/unity.c`) on the new game's host
generation or an opt-in env var, so a shipped port's code path is byte-for-byte unchanged; say so
explicitly. Do not propose "this would also fix TR2" as a benefit that implies touching it. Also: the user
**prefers systemic fixes over per-game workarounds** ("other games may need a more systemic fix") — when a
subsystem fails (e.g. Unity/FMOD music), dig for the root cause before reaching for an outcome-only
workaround. Related: [[deliver-the-outcome-not-the-mechanism]], [[systematic-not-brute-force]].
