# Stage 1 — Lifecycle + UI/GL thread split

**Objective:** give the runtime a **real Activity/GLSurfaceView lifecycle and the UI-thread/GL-thread
split** as the *default* model, so the engine sees the frame boundaries and callback ordering Android
guarantees. This is the seam under Input, Orientation, and Audio.
**Architecture ref:** `../android-runtime-architecture.md` §1 (lifecycle row), §3a, Phase 1.
**Depends on:** Stage 0.
**Status:** ☐ Not started

## 1. The Android 2.3 contract (the spec to honor)

- **Activity lifecycle ordering:** `onCreate → onStart → onResume`, and for the GL surface
  `surfaceCreated → surfaceChanged(format,w,h)` *before* the first frame; on backgrounding
  `onPause`/focus-loss, on foreground `onResume`/regained-focus. Games gate input and sim on these.
- **Two threads, by construction:** a `GLSurfaceView` runs the renderer on a **dedicated GL thread**
  (`onSurfaceCreated/onSurfaceChanged/onDrawFrame`); **`onTouchEvent` and lifecycle callbacks fire on
  the UI/main thread** via the Looper. The engine therefore receives **input on the UI thread** and
  **renders on the GL thread**, with its own queue+mutex bridging them. This split *is* the contract —
  it produces the natural ≥1-frame gap between a finger being registered and being processed.
- WMW-specific corroboration: `libwmw` does **0 `pthread_create`** but uses `pthread_mutex` +
  `pthread_cond_wait/broadcast` — i.e. it *expects the framework to supply both threads* and
  synchronizes across them. Our single-thread puppeteer violates this.

## 2. Current state in apkenv (as-built)

- `apkenv.c` main loop is single-threaded: `input_update → module->update (drawFrame) → platform->update`.
- `modules/wheresmywater.c` has an **optional** UI thread (`APKENV_WMW_UITHREAD`, default on) that
  drains a touch queue on a separate pthread — but it still dispatched DOWN+MOVE **back-to-back** (no
  enforced frame gap), and it's WMW-scoped + env-gated. Right idea, wrong layer.
- Lifecycle is a fixed boot sequence in the module (`rendererInit → resized → onGameResume →
  onRegainedFocus`); there is no general `onPause`/`onResume`/surface model. `regainedTop()` is only
  reached via the accidental task-switch path.

## 3. Work items

1. Promote the two-thread model into the **platform/runtime** (not the module): a **GL thread** owns
   GL context + `module->update` (render+sim); a **UI thread** owns SDL/PDL event polling + lifecycle
   callbacks + the input queue. Make this the default; remove the env gate.
2. Define a runtime **lifecycle API** the module maps its entrypoints onto:
   `create / surface_created(w,h) / resume / focus_gained / focus_lost / pause / destroy`. Drive the
   correct ordering centrally; the module just binds each to the engine's call
   (`onGameResume/onRegainedFocus/onGamePause/onLostFocus/backKeyPressed`).
3. Wire webOS app lifecycle (PDL/SDL `ACTIVEEVENT`, task-switch) to the lifecycle API so resume/pause
   are real, not accidental.
4. Establish the **input handoff primitive** the cadence depends on (consumed by Stage 2): UI thread
   enqueues events with their arrival frame; GL thread drains at the top of each frame; the queue
   guarantees an event enqueued during frame N is **not drained before frame N+1** (the Android gap).
   Leave the *event semantics* (multi-pointer encoding) to Stage 2 — Stage 1 only owns the threads,
   ordering, and the gap.

## 4. Verification gate (on-device, pass/fail)

- [ ] Trace (Stage 0) shows `module->update` running on a **distinct thread id** from SDL/PDL polling.
- [ ] Trace shows an event enqueued in frame N is drained in **frame ≥ N+1** (the gap exists by
      construction, not by luck).
- [ ] Task-switch → pause → resume restores play **without** any WMW-specific special-casing (the
      menu/pause overlay path still works; resume returns to gameplay cleanly).
- [ ] Menus remain interactive (no regression).

## 5. De-hack / cleanup when green

- Delete `APKENV_WMW_UITHREAD` (now the default model). The module no longer spawns its own thread.

## 6. Review checklist (for an external reviewer)

- [ ] Is the GL context created/used on **exactly one** thread for its whole life (GLES contexts are
      thread-affine)? Confirm no GL call happens on the UI thread.
- [ ] Is the enqueue→drain frame-gap a **guarantee** (queue handed off across the frame boundary), or
      an emergent timing accident that could disappear under load?
- [ ] Is lifecycle ordering centrally enforced, or re-implemented per module? (Per-module = wrong.)
- [ ] Are mutex/cond handoffs free of races between UI enqueue and GL drain? (Engine uses cond_wait —
      verify we signal it the way the framework would.)

## 7. Risks / open questions

- GLES context thread-affinity: moving render to a dedicated thread must keep the SDL/PDL GL context
  on that thread. The webOS 3-layer compositor + SDL GL ownership (no raw EGL) must be respected from
  the GL thread.
- The "gap" must be a real handoff, not a `sleep`. Tie it to the frame boundary.

## 8. Work log
