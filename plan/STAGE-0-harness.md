# Stage 0 — Conformance & trace harness (+ the input-cadence analysis run)

**Objective:** turn the throwaway inline-hook debugging into a **permanent, reusable
Android-contract probe** — a structured trace channel plus a small set of conformance checks — and use
it immediately to **settle the input-cadence root cause** before any Stage-2 code is written.
**Architecture ref:** `../android-runtime-architecture.md` §1 (diagnosis), §3a, Phase 0.
**Depends on:** —
**Status:** 🔬 Analysis run **DONE** (root cause found 2026-06-27, §8); reusable-harness generalization still pending.

This stage writes almost no production code. Its value is **replacing "poke and squint" with
"measure against the contract."** Every later stage's gate is observed through this harness.

## 1. What this stage must produce

There is no Android contract to honor here; the deliverable is *measurement capability*:

1. A **structured trace channel** — labeled, low-overhead, runtime-gated (one env var, default off
   for release), writing tagged records (the existing `[WMWTOUCH]`/`[WMWHOOK]`/`[SDLEV]` markers
   formalized into a stable schema: `tag | frame | thread-id | wall-ns | payload`).
2. A reusable **inline-hook facility** generalized out of `modules/wheresmywater.c`'s
   `wmw_install_hook` (mprotect + trampoline) into the runtime so any subsystem (not just WMW) can
   instrument an engine address by symbol/offset.
3. **Conformance probes** — assertions that check a contract is being honored and log a PASS/FAIL:
   - input: event-lifecycle ordering + per-frame cadence (see the analysis run below)
   - orientation: the self-consistency of the reported frame (Stage 3 §1 checklist) in one dump
   - audio: ring-buffer underrun/overrun counters (Stage 4)

## 2. Current state in apkenv (as-built)

- `modules/wheresmywater.c` already has `wmw_install_hook()` (mprotect, 2-insn trampoline) and ~13
  trace tags, gated by `APKENV_WMW_HOOK` / `APKENV_SDL_TRACE`, all `fprintf(stderr,…)` → `/tmp/wmw.log`.
- `platform/webos.c` has an SDL-event heartbeat (`[SDLHB]`, every 120 calls) and event log (`[SDLEV]`).
- These are WMW-scoped and ad-hoc. Generalize, don't rewrite.

## 3. Work items

1. Extract `wmw_install_hook` + trampoline into a runtime facility (e.g. `debug/trace.c` already
   exists — fold into it): `trace_hook(void *addr, void *fn)` and `trace_emit(tag, fmt, …)` with the
   stable record schema and the **thread id** + a **monotonic frame counter** + **wall-ns** in every
   record (these three are what the input analysis needs).
2. One env var to gate all tracing (`APKENV_TRACE=0/1/2`), default 0. Keep per-area filters.
3. A capture helper (device-side `.sh`) that runs a labeled session and pulls the log.
4. **The input-cadence analysis run (the real point of this stage):** with hooks on
   `WidgetManager::touchDown` (`0x1edc10`, log the finger-map-hit vs create branch and whether
   `0x1ede08` hit-test runs), `Widget_FingerCatcher::acceptNewFingerEntered` (`0x1fe7a4`), and the
   per-frame `WidgetManager::update` enter-detection, capture and diff **a working menu tap**
   (`Screen_LevelSelect`) vs **a dead gameplay drag** (`Screen_WaterTest`). Answer, with the trace as
   evidence:
   - On the menu path, is there a **frame boundary** between the finger's create (DOWN) and the event
     that hit-tests/catches it? On the gameplay path, is that boundary **absent** (DOWN+MOVE collapsed
     into one frame)?
   - Does `acceptNewFingerEntered` require the finger to be present for **≥2 `update()` calls**?
   - Does the catch fire on `WidgetManager::touchDown` for an *already-mapped* finger (i.e. a 2nd
     event for the same id), or only via `update()` enter-detection?

## 4. Verification gate (on-device, pass/fail)

- [ ] `APKENV_TRACE=1 ./play.sh` emits a labeled trace with `thread-id + frame + wall-ns` per record,
      and `APKENV_TRACE=0` produces a clean (trace-silent) run.
- [ ] **The analysis run yields a written, evidence-backed answer** to the three questions in work
      item 4 — i.e. the precise frame/thread/event condition the engine's catch waits for. This answer
      becomes Stage 2 §1's confirmed spec.

## 5. De-hack / cleanup when green

- Replace the ad-hoc `[WMW*]` `fprintf`s with the unified `trace_emit`. Remove `/tmp/wmw.log` hardcoding.
- Keep the facility (it's permanent); only the WMW-specific *probes* are removed once their stage lands.

## 6. Review checklist (for an external reviewer)

- [ ] Is the trace record schema stable and does **every** record carry thread-id + frame + wall-ns?
      (Without those three, the input analysis is not decidable.)
- [ ] Does the analysis run actually **compare menu vs gameplay** on the *same* build, or could the
      difference be a build artifact?
- [ ] Is the conclusion about the catch precondition supported by the trace, or asserted? Point to the
      specific records.
- [ ] Is tracing truly zero-cost when `APKENV_TRACE=0` (no hooks installed, no per-frame work)?

## 7. Risks / open questions

- gdb-over-SSH was unreliable (silent detaches; the gdb-launched instance received 0 input). **Do not
  depend on gdb** — the in-process hook trace is the supported path.
- The analysis might show the precondition is *not* a frame-gap (e.g. it's an `ACTION_POINTER_DOWN`
  vs `ACTION_DOWN` distinction, or a 2nd-event-for-same-id requirement). That's a success — it
  redirects Stage 2 cheaply instead of after a wrong build.

## 8. Work log

### 2026-06-27 — analysis run COMPLETE; root cause found (overturns the prior belief)
Method: static RE of `WidgetManager::{touchDown(0x1edc10), touchMoved(0x1ea99c), update(0x1ece38)}`
from `wmw-patch/libwmw.base.so`, mined `last-wmw.log`, then ONE on-device run of a new `[WMWUPD]`
probe (hook on `WidgetManager::update`, dumps finger-0's map state each in-level frame). No gdb.

**Eliminated (proven):**
- `touchMoved` **never captures** — it only updates `FingerInfo` position. ❌ "catch on first move".
- `update()`'s `dt` arg is **written once, never read**. ❌ "wrong dt" for touch.
- Coordinates fine: in-level `AABB::contains` is called with sane portrait coords and **HITs** the
  play area (FingerCatcher infinite box + a full-screen box). ❌ coordinate-space.
- Delivery / manager gate / threading / cadence — all fine; finger reaches the level WidgetManager.

**Root cause (definitive):** the in-level finger **IS captured** — by a `Walaber::Widget_PushButton`
(runtime `0x5afee8`, stored vtable `0x469928` = PushButton vtable `0x469920`+8), **not** the carve
`Widget_FingerCatcher` (`0x565ff8`, vtable `0x469618`; its `handleEvent` fires with `count[+12]=0`
forever). `[WMWUPD]` shows finger 0 `captured=0x5afee8`, `state=1/3`, every stroke. Mechanism: the
only fresh-finger capture path is `update()` "Phase 2" (uncaptured + `FingerInfo+0==0` → walk widgets
→ record those whose AABB contains the point into an entered-tree keyed by widget id → dispatch
`acceptNewFingerDown` **in tree order**, first non-zero wins + exits). Both the PushButton and the
FingerCatcher contain the finger; the **PushButton sorts first, captures, loop exits → FingerCatcher
starved**. So `[WMWCATCH]=0`/`[WMWENTER]=0` because the FingerCatcher’s accept fns are simply never
reached — and the old force-catch stopgap was masking a **capture-arbitration** bug, not a no-capture
bug.

**Implication for the plan:** this is **widget-tree STATE** (a full-screen/overlay PushButton that is
present + enabled + sorts ahead of the carve surface during active play), i.e. a **lifecycle / init-
state gap (Stage 1)** from skipping the real Activity/View init — NOT touch-delivery mechanics. Stage 2
as written (multi-pointer MotionEvent) is still correct hygiene but is **not** what unblocks WMW carve.

**Analysis gate: PASSED** (the precondition question is answered). Open follow-up (small): identify
*which* PushButton (`WidgetManager::getWidget` id) + its bounds + why it’s present during play
(tutorial/overlay left active?) to pin the exact lifecycle cause.
<!-- date — what changed — evidence (trace excerpts, gate result) -->
