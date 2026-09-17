---
name: systematic-not-brute-force
description: User wants theory-driven, instrumented, staged work on the device port — not try/deploy/touch loops; on-device tests only for well-prepared theories
metadata:
  type: feedback
---

For the webOS game-port work, the user explicitly rejected the "try something, deploy, touch, scream, try something else" loop (2026-08-26). They want: a systematic review first, explicit theories ranked by evidence, instrumentation that yields signals from the device log, and their own time used only for solid, carefully prepared tests with a written protocol and expected outcomes.

**Why:** the user is the only one who can touch the device; each round-trip costs them real time and patience ("You may work like a robot, but I cannot"). Brute-forcing PvZ HD burned days patching the wrong binary.

**How to apply:** before asking for a device test, (1) derive the contract statically (decompile the Java host, diff engine→host calls vs the module), (2) write the theory + expected log signals into a plan doc (e.g. `plan/PVZ-HD-menu-freeze.md`), (3) build instrumentation that names the next gap on failure, (4) hand over ONE change at a time with a 3-minute protocol. Related: [[trust-operator-diagnosis]], [[wrapper-spike-progress]].
