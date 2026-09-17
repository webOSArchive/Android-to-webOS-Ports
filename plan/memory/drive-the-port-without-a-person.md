---
name: drive-the-port-without-a-person
description: Synthetic touch (APKENV_MORTAR_AUTOTAP) + tools/grab.sh lets a port be driven through menus and gameplay with nobody holding the device
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b8d186fb-77d5-48ec-aeb3-61d9b159a425
  modified: 2026-09-15T14:43:58.463Z
---

When the operator is away and a dev device is connected, a port does not have to stop at "it boots".
Add an env-gated synthetic-input path to the module — `APKENV_MORTAR_AUTOTAP="x,y@frame;x1,y1>x2,y2@frame"`
in `apkenv/modules/mortar.c` is the template — that presses, drags and releases through the module's
own `input()` callback, and read the result with `apkenv/tools/grab.sh`.

**Why:** it goes through the *real* contract (normalized 0..1 coords, Android action codes,
`ACTION_POINTER_DOWN` for a second finger), so it proves the touch path rather than bypassing it —
and it turns "taps do nothing" from an anecdote into a reproducible run. On Fruit Ninja
(2026-09-15) it took the port from a first-run screen to a scoring Classic game — menus, mode
select, slicing, bombs — in four device cycles with nobody in the room.

**How to apply:** keep it diagnostic — env-gated, off by default, and never in a shipped
`apkenv.env` (the ship check unpacks the `.ipk` and reads that file). It does **not** replace the
operator: SDL only delivers real input to a package launched from its icon, and audio being provably
fed is not the same as audible. Report those as open, not as verified.

Related: [[systematic-not-brute-force]] (the person holding the device is the scarce resource),
[[screenshots-lie]] (always `grab.sh`, never the on-device screenshot),
[[call-the-engines-jni-onload]], [[wrapper-spike-progress]].
