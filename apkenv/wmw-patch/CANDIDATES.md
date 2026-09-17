> **Historical (2026-06).** These in-game touch patch candidates were never needed: touch was fixed
> host-side (`PDL_Init` before `SDL_Init`, normalized 0..1 coordinates). The shipped build carries only
> the `std::string(NULL)` fix, which `bake.py` applies from the pristine apk.

# In-game touch fix — bake candidates (pending probe #1)

Goal: make Widget_FingerCatcher actually CATCH our finger so the carve fires
(and HUD buttons capture). Root cause + offsets: see ../BUILD-STATE.md.

Tooling: `python3 bake.py` applies `PATCHES` (in bake.py) to libwmw.base.so and
writes `wheresmywater_patched.apk` (carries the std::string fix + new patch).
Deploy: push that apk to device `/media/internal/wheresmywater.apk`, then play.sh.
All patches are length-preserving 4-byte ARM words; bake.py verifies the original
word before writing. (.text file-offset == vaddr for this .so, confirmed.)

## Probe #1 decides the lever
Run the probe build (currently deployed), enter a level, swipe + tap buttons.
Read `[WMWWMTD]` and `[WMWCATCH]`:

### Outcome A — `[WMWWMTD] ... NOT-FOUND -> CREATE path` (expected)
touchDown never hit-tests a fresh finger, so acceptNewFingerDown is never called.
Candidate patches (try in this order, one at a time, re-test each):

- **A1 — touchMoved always marks the finger "active".** WidgetManager::touchMoved
  @0x1eaa08 does `ldr r1,[ip]; cmp r1,#0; movne r1,#1; strne r1,[ip]` (state goes
  0->stays 0). Patch the `cmp/movne/strne` so it unconditionally sets state=1, in
  case WidgetManager::update's enter-detection skips state==0 fingers. Low risk
  (only changes a touched finger's state byte). **ENCODED + READY** (commented in
  bake.py): 0x1eaa0c e3510000->e3a01001 (cmp->mov r1,#1); 0x1eaa14 158c1000->
  e58c1000 (strne->str). To try: uncomment those 2 lines, `python3 bake.py`,
  push wheresmywater_patched.apk to device /media/internal/wheresmywater.apk.

- **A2 — make touchDown's create path fall through to the hit-test.** After the
  CREATE path stores the new FingerInfo (~0x1eddf4) it returns at 0x1eddf8; the
  hit-test entry is 0x1ede08. Redirect create-end -> 0x1ede08 so a fresh down also
  hit-tests/catches. HIGHER risk: 0x1ede08 expects the found-node in a register
  (r6) and wm in r8; must verify the create path leaves r6 = the new node and r8 =
  wm before branching. Disassemble 0x1edd70..0x1ede40 and trace r6/r8 first.

### Outcome B — `[WMWWMTD] ... FOUND ... -> HIT-TEST path` but `[WMWCATCH]` still 0
touchDown DOES hit-test but the FingerCatcher isn't caught there. Then the
FingerCatcher's AABB check in the hit-test (or a higher widget capturing first) is
the issue — investigate the hit-test loop (0x1ede08..0x1edff0) and which widget
vtable+0x3c gets the down. Patch target TBD from that.

### Outcome C — `[WMWCATCH] _acceptFinger` DOES fire but carve still dead
Then catch works; the failure is downstream (World sim / _screenToWorld / level
"active" state). Re-target World::handleTouchMoved (0x2c3c38) internals.

## Notes
- Prefer a SHIM-level fix if one emerges (no binary patch) — e.g. if the probe
  reveals a specific FingerInfo field/flag we can set from the module each frame.
- Keep candidate patches minimal + reversible; bake.py's original-word check is
  the guardrail against wrong offsets.
