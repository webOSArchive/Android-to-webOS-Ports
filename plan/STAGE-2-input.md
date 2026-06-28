# Stage 2 — Input contract (faithful multi-pointer MotionEvent)

**Objective:** replace the pre-2.0 single-pointer input model + the WMW force-catch stopgap with a
**faithful Android 2.3 multi-pointer MotionEvent + InputDispatcher**, delivered with the cadence and
thread split Stage 1 provides, so engines' own per-finger logic works **with no per-game pokes**.
**Architecture ref:** `../android-runtime-architecture.md` §3a, Phase 2.
**Depends on:** Stage 0 (the confirmed cadence root cause), Stage 1 (thread split + frame gap).
**Status:** ☐ Not started

## 1. The Android 2.3 contract (the spec to honor)

**MotionEvent action encoding (load-bearing — these values matter):**
- `action` int = `(pointerIndex << ACTION_POINTER_INDEX_SHIFT) | actionMasked`.
- `ACTION_MASK = 0xff`, `ACTION_POINTER_INDEX_MASK = 0xff00`, `ACTION_POINTER_INDEX_SHIFT = 8`.
- `ACTION_DOWN=0, ACTION_UP=1, ACTION_MOVE=2, ACTION_CANCEL=3, ACTION_OUTSIDE=4,
  ACTION_POINTER_DOWN=5, ACTION_POINTER_UP=6`.
  (apkenv.h currently defines only `DOWN/UP/MOVE` and wrongly aliases `MOVE`=`MULTIPLE`=2 — fix it.)

**Gesture lifecycle:** first finger → `ACTION_DOWN`; each additional finger → `ACTION_POINTER_DOWN`
(index in the high byte); movement → `ACTION_MOVE` (may batch multiple historical samples per event);
a non-last finger lifting → `ACTION_POINTER_UP`; the last lifting → `ACTION_UP`. **Pointer ids are
stable** for a finger's entire lifetime; `getPointerId(index)` maps slot→id.

**Delivery:** on the UI thread; the engine drains on the GL thread (Stage 1). The natural consequence
the WMW catcher depends on: a finger is **registered on DOWN** and **hit-tested/caught on a later
event or a later `update()` frame** — never both within one collapsed step.

> **Confirm Section 1 against the Stage-0 analysis run before coding.** If the analysis showed the
> precondition is specifically "finger present for ≥2 `update()` calls" or "a 2nd event for an
> already-mapped id triggers the hit-test," encode *that exact* condition here.

## 2. Current state in apkenv (as-built)

- `platform/common/input_transform.c` = per-game 2D coordinate flips ("custom rotation hack for world
  of goo"), not a coordinate-frame transform. To be replaced.
- `modules/wheresmywater.c` synthesizes touch into `rendererTouchBegan/Moved/Ended(count,xs,ys,ids)`;
  default "legacy" mode pins finger id 0, count 1. Coordinate delivery is **faithful** (matches
  `WMWView.copyTouches`: normalized 0..1). The problem is **lifecycle/cadence**, not coords.
- Carve only works via `APKENV_WMW_FORCECATCH` (manually calls `Widget_FingerCatcher::_acceptFinger`
  @ `0x1fe428` each frame) — the stopgap this stage removes.
- Failed experiments (do not repeat — see `apkenv/HANDOFF-touch-water.md`): multitouch aggregation,
  UI-thread split *without* the frame gap, `regainedTop()` on entry, force manager gate, double-DOWN
  (engine dedups), enter-catch gate set, REHIT/REHIT2.

## 3. Work items

1. Define `AndroidMotionEvent` in the runtime: pointer count, per-pointer `{id, x, y}` arrays, the
   `action` int with the encoding above, and batched MOVE history. Add the missing action constants to
   `apkenv.h`; remove the `MOVE`/`MULTIPLE` alias.
2. Build the **InputDispatcher**: SDL multi-mouse (`event.{button,motion}.which` 0–4, per
   `webos://knowledge/pdk`) → per-pointer state machine emitting `DOWN / POINTER_DOWN / MOVE /
   POINTER_UP / UP` with **stable ids**. PDL init order already correct (PDL before SDL, aggression
   MORETOUCHES, gestures off — keep).
3. Deliver through the Stage-1 UI→GL handoff so **DOWN and the first MOVE land on different frames**
   (reproduce the gap the catcher needs). The per-game module's `rendererTouch*` calls are driven by
   the dispatcher on the GL-drain, *not* synthesized ad-hoc.
4. Per-game module shrinks to a **mapping**: "this engine's touch entrypoints are X/Y/Z, coords are
   normalized 0..1." No catch logic, no thread, no env knobs.
5. Keep the Stage-0 probes on `acceptNewFingerEntered`/`_acceptFinger`/`WidgetManager::touchDown` as
   the gate instrument; remove them at de-hack.

## 4. Verification gate (on-device, pass/fail)

- [ ] With **`APKENV_WMW_FORCECATCH=0`**, in `Screen_WaterTest`: trace shows
      `Widget_FingerCatcher::acceptNewFingerEntered` (or `_acceptFinger`) **fires for our finger**,
      `catcher+0xbc` catch-count > 0, and `World::handleTouchDown` runs.
- [ ] Carve-by-drag works **and** in-game HUD buttons (pause/reset) capture — a level is completable
      with no force-catch.
- [ ] Menus still navigate/launch (no regression).
- [ ] Multi-finger: a 2nd finger produces `ACTION_POINTER_DOWN` with a distinct stable id (trace).

## 5. De-hack / cleanup when green

- Delete force-catch and **all** input env knobs: `APKENV_WMW_{MULTITOUCH, FORCECATCH, CALLREGAIN,
  FORCEINPUT, DOUBLEDOWN, ENTERCATCH, REHIT, REHIT2}` (and `UITHREAD`, removed in Stage 1).
- Delete `platform/common/input_transform.c`'s per-game flips (coords now come from the dispatcher +
  Stage-3 orientation transform).

## 6. Review checklist (for an external reviewer)

- [ ] Does the dispatcher emit `ACTION_POINTER_DOWN/UP` (5/6) with the **index in the high byte**, or
      does it collapse everything to DOWN/UP/MOVE? (Collapsing = not faithful.)
- [ ] Are pointer **ids stable** across a gesture, and reused only after UP?
- [ ] Is the DOWN→MOVE frame gap a **structural guarantee** from Stage 1, or re-introduced as a hack
      here?
- [ ] Is the carve fix a **general** consequence of faithful delivery, or did a WMW-specific shortcut
      sneak back in? (Grep the module for hardcoded vtable offsets / `_acceptFinger`.)
- [ ] Does the fix match the **Stage-0 measured** precondition, or a guessed one?

## 7. Risks / open questions

- If, after faithful delivery, the catcher *still* needs a nudge, the Stage-0 analysis was incomplete
  — return to measurement, don't reinstate force-catch.
- ACL ran the engine's actual Java `onTouchEvent`/`copyTouches`; we approximate it. If an engine reads
  raw `MotionEvent` fields we don't populate, extend `AndroidMotionEvent` rather than special-casing.

## 8. Work log

### 2026-06-27 — frame-paced delivery implemented; cadence FIXED, arbitration remains
**Built (general, default-on, `APKENV_WMW_PACE`):** dispatch at most ONE touch transition
per finger per engine frame (DOWN frame N, MOVEs coalesced, UP held to a later frame), from
the GL thread before `rendererDrawFrame`. `modules/wheresmywater.c`: `FingerPace` FSM +
`wmw_pace_record`/`wmw_pace_advance`; UI-thread dispatch disabled in paced mode.

**Why:** proven on-device that a tap's DOWN+UP were landing on the SAME engine frame
(`[WMWTOUCH] f1723 BEGAN + f1723 ENDED`), so the per-frame `WidgetManager::update` (Phase-2
capture) never ran between them → finger ABSENT from the map at update time (1485/1485
samples) → no capture at all.

**Result (on-device):** pacing works — DOWN/UP now on separate frames (`f650 BEGAN→f652
ENDED`), and the finger is **now PRESENT + captured** at update time (`captured=…, state=1`).
The cadence bug is fixed and it's a **general** fix (any engine with a per-frame widget
capture). Menus still navigate. **KEEP this.**

**Remaining (the actual carve blocker, = Stage-0 arbitration, now reachable):** the captured
widget is `Walaber::Widget_PushButton` (vtable `0x469928`) — the full-screen **"press Swampy"**
widget (`handleEvent a=10` → `SwampyPressOnLevel`), NOT the carve `Widget_FingerCatcher`
(`0x469618`, fires every frame with `count=0`, never catches). Static RE:
`PushButton::acceptNewFingerDown` accepts the first free finger unconditionally;
`releaseFingerMoved` returns 0 (a drag on it does nothing). The capture winner is the first
widget in the Phase-2 entered-tree, keyed by `widget+0x10`; in our build the PushButton sorts
ahead of the FingerCatcher. On Android the carve works, so the FingerCatcher must sort first
there → the difference is **widget-tree state/order set during level build** (creation
order / `widget+0x10`), i.e. a **lifecycle / async-load-sequencing** gap (Stage 1), not a
delivery/cadence gap. Force-catch (Stage-0 stopgap) still beats it; the systematic fix needs
the faithful level-build/lifecycle ordering or a determination of what `widget+0x10` encodes.

### 2026-06-27 (cont.) — arbitration root cause nailed: async widget-load ORDER
On-device `[WMWARB]` probe (both competitors' fields at capture): thief PushButton `+0x10=0,
layer=1, en=1`; carve FingerCatcher `+0x10=5, layer=1, en=1`. Same layer, both enabled — the
carve loses purely on the entered-tree key `widget+0x10` (dispatch = ascending key, first-accepter
wins, so key 0 beats key 5). Static RE: `WidgetManager::addWidget(Widget*, int)` (0x1ea43c) does
`str r6,[r4,#16]` → **`widget+0x10` = the int key the caller passes**, and the callers are the
engine's **async widget-load callbacks** (`WidgetHelper::_fileReadCallback` 0x1fa894,
`Screen_*::_finishedLoadingWidgets`). So the key is set by the order/timing the async widget-load
completes. Same `libwmw` runs on Android where carve works ⇒ our async-load order differs,
flipping button-vs-carve priority. Two candidate sub-causes, both **lifecycle (Stage 1)**:
(a) our async file-read/deferred-load completes in a different order (fopen-redirect /
sync-vs-async reads); (b) the full-screen button is a level-1 **tutorial** element Android removes
after the tutorial that our lifecycle leaves present. Decisive cheap test: carve on a
**tutorial-free level** — works ⇒ (b); fails everywhere ⇒ (a). No general apkenv-layer knob (it's
all inside libwmw); the only non-libwmw-patch fix is faithful async-load ordering / lifecycle.

### 2026-06-27 (cont.) — keys are CONTENT-FIXED ⇒ it's state/presence, not load-order
`[WMWADD]` probe (hook on `addWidget`, logs every widget's key+class+order during level load):
keys are **fixed per class, not a running counter** — `Widget_FingerCatcher` is **always key=5**
(70×), `Widget_PushButton` **always key=0** (62×), `Widget_Label` 0/2, etc. So the arbitration
order is **identical on Android** (button key 0 < carve key 5). Therefore the carve works on
Android only because the **full-screen key-0 Swampy `Widget_PushButton` is NOT capturing there**
during free play — i.e. this is a **widget state/presence** bug (a full-screen, enabled, key-0
button active in our build when it should be removed/disabled/non-full-screen), NOT load-order and
NOT a delivery/cadence gap. The thief always accepts a free finger (`acceptNewFingerDown` returns 1
unconditionally for a free finger) and `releaseFingerMoved` returns 0, so it silently eats drags.
Open: is the full-screen key-0 button **level-1-tutorial-specific** (→ runtime already correct for
normal levels; fix = tutorial-state progression) or **present on every level** (→ a permanent
state/visibility gate our lifecycle misses)? Decisive test: carve on a non-level-1 level.

### 2026-06-27 (cont.) — CONFIRMED + in-level touch SOLVED ✅
On-device: carve fails on **level 2** too (no tutorial) ⇒ the thief is **present on every level**,
not tutorial-specific. Its AABB is `0,0..768,1024` (full portrait screen) — the contradiction
(content-fixed keys yet Android carves) resolves to **the thief's bounds**: it's full-screen here
but localized on Android, so a play-area drag misses it there and the carve catcher wins. A
full-screen key-0 button also explains the dead HUD buttons (it grabs the single key-0 entered-tree
slot before they can).

**KILL-THIEF experiment (`APKENV_WMW_KILLTHIEF=1`, launcher `play-kill.sh`):** when a finger is
captured by a full-screen (`>700×>900`) key-0 `Widget_PushButton` (vt 0x469928), disable it
(`+0x60=0`, skipped in the contains-walk) and release the finger. **On-device: carve AND HUD
buttons both work.** ⇒ the full-screen PushButton is conclusively the **sole** in-level blocker;
everything else (cadence, capture machinery, carve, HUD) is correct.

**WMW is now playable end-to-end** (FBO water + frame-paced delivery + thief neutralized).
Remaining = make the thief fix precise/clean rather than heuristic: identify *what* that button is
(fires `SwampyPress`/`handleEvent a=10` → the "press Swampy" widget) and *why* its bounds are
full-screen / why it doesn't hand drags to the carve on Android (its `releaseFingerMoved` returns 0).
The current kill-thief disables the button (loses the tap-Swampy feature) and is heuristic
(vtable + full-screen check); the clean fix corrects its bounds/release-on-drag behavior.
