# apkenv webOS port — BUILD STATE (persistent; do NOT keep this only in chat/scratch)

## ⭐ SESSION END STATE — 2026-06-28 — AUDIO ✅ VERIFIED ON DEVICE + PACKAGING SCAFFOLDED (resume here)

**Audio (Stage 4) WORKS on the TouchPad — user-confirmed audible (2026-06-28).**
WMW now runs end-to-end on webOS WITH SOUND (portrait FBO water + frame-paced
carve/HUD touch + FMOD AudioTrack pump). Source committed (`git log`:
"Audio (Stage 4): FMOD AudioTrack device pump..."). The **new known-good binary
md5 `130fd02b` is deployed at `/var/apkenv/apkenv`**; the prior pre-audio
playable build is saved as `/var/apkenv/apkenv.prev` (revert: `cp apkenv.prev apkenv`).
On-device facts: FMOD mix rate read dynamically = **24000 Hz stereo** (not 44100);
SDL opened `ring=32768 sdlframes=512`, DSP `dspLen=512 dspNum=4 chunk=2048`;
mixer stable (single init/open, no errors). webOS SDL 1.2 audio backend works.
Run `sh /var/apkenv/play.sh`; logs `/tmp/wmw.log` (`[FMOD]`/`[AudioTrack]` lines).
Device reachable via SSH at .88 again (was a transient route hiccup); novacom-USB
also works (`novacom run file:///bin/sh ...`).

**What was built (all general — no WMW-specific audio constants):**
- `audio/audiotrack.{c,h}` — `android.media.AudioTrack`-style S16 streaming sink
  over a lock-free SPSC ring drained by the SDL pull callback.
- `audio/fmod_pump.{c,h}` — C re-implementation of `org.fmod.FMODAudioDevice.run()`
  (no Dalvik runs the Java audio thread). Resolves libfmodex's
  `Java_org_fmod_FMODAudioDevice_{fmodGetInfo,fmodProcess}` via `lookup_symbol`,
  pumps `fmodProcess→audiotrack_write` on a dedicated thread.
- `jni/jnienv.c` — `NewDirectByteBuffer`/`GetDirectBufferAddress`/`...Capacity`
  implemented (were stubs); `fmodProcess` needs `GetDirectBufferAddress`.
- `platform/common/sdl_audio_impl.h` — NULL `obtained` so SDL converts to our
  desired format (else wrong-pitch on hw).
- `modules/wheresmywater.c` — pump start/stop on init/resume / pause/deinit.
  Audio default ON; `APKENV_WMW_AUDIO=0` disables.

**Validation done offline (device unavailable this session):** 8M-sample
producer/consumer ring stress test (real `audiotrack.c`, stubbed SDL) →
no corruption, no loss, underruns zero-filled; **ThreadSanitizer clean**.
Adversarial review done + fixes applied. Full trail: `../plan/STAGE-4-audio.md` §8.

**NEXT (needs device): redeploy `apkenv`, run `play.sh`, listen** — menu music +
carve/UI SFX audible & in sync; no dropouts over minutes; pause/resume clean.
Watch the `[FMOD]`/`[AudioTrack]` log lines (rate/dspLen/dspNum/underrun). First
risk: webOS SDL 1.2 audio backend opening at all + honoring the FMOD mix rate
(24000 vs 44100 — the pump reads it from `fmodGetInfo`, not hardcoded).

**Packaging (Stage 6) ✅ DONE — WMW launches from the TouchPad LAUNCHER ICON
(2026-06-28).** `.ipk` (id `com.apkenv.wheresmywater`) installs via `palm-install`
over novacom and launches from the launcher; jailed app boots, GL on Adreno 220,
FMOD audio (rate=24000), smooth render loop. Build: `scp` the device bionic libs
into `packaging/staging-libs`, then `./packaging/build-ipk.sh` →
`packaging/out/*.ipk`. Reinstall needs a version bump in `appinfo.json`
(`palm-install` refuses same/lower).

**TouchPad PDK launch path (hard-won; not in the webOS-MCP docs — found by
catching the live pid):** LunaSysMgr runs PDK apps in a **hybrid jail**
(`/var/palm/jail/<appid>/`) as **uid 5003**, `LD_PRELOAD=libpvrtc.so`;
`/media/internal` is bind-mounted **rw** into the jail (so game data there is
visible — only the binary needs the exec app partition; apk libs load via
apkenv's own anon-RWX linker). Gotchas, all handled in `apkenv.c` `#ifdef
__webos__`: (a) sysmgr passes the **launch-params JSON as argv** (`apkenv "{ }"`)
→ force `APKENV_DEFAULT_APK`; (b) **`main` must be the ARM binary** (a shell
script is not exec'd); (c) a jailed PDK app's **stdio goes nowhere** → redirect
to `/media/internal/apkenv-wmw.log` from inside the binary; (d) **`argv[0]`
unreliable** → self-locate via `/proc/self/exe`, chdir to app dir for
`./libs/webos`; (e) jail is **torn down on exit** + **can't ptrace** → read the
in-jail log live via `/proc/<pid>/root/...`. **SELF-CONTAINED (2026-06-28):** the .ipk now BUNDLES the game apk — fresh device
needs nothing pre-staged. Verified: cleaned all cruft (`/var/apkenv`, `gameroot`,
the loose `/media/internal/wheresmywater.apk`), installed v1.0.1, launched from
the launcher → boots from the BUNDLED `android/wheresmywater.apk` with GL+audio,
nothing on `/media/internal` but its own savegame dir `/media/internal/.apkenv/`.
App-dir layout (organized for re-packers): `apkenv` `appinfo.json` `icon.png`
`README`; `libs/webos/` = webOS host libs; `android/<game>.apk` = the Android
bits (packaged launch auto-finds the first *.apk there). Build: pull bionic libs
→ `packaging/staging-libs`, put the patched apk at `packaging/wheresmywater.apk`
(or `$APK=...`), `./packaging/build-ipk.sh`. Full trail: `../plan/STAGE-6-packaging.md` §7.

---

## SESSION END STATE — 2026-06-27 — WMW PLAYABLE END-TO-END

**Where's My Water now boots, renders correct upright PORTRAIT with correct water, and is fully
touch-playable (carve-by-drag + HUD buttons) on the TouchPad.** Deployed build at
`/var/apkenv/apkenv` (md5 `dbca1a7c...`; previous saved as `apkenv.prev`).
**Run: `sh /var/apkenv/play.sh`** — a plain run is now fully playable; FBO render + frame-paced
touch + thief-neutralize are all DEFAULT-ON in `modules/wheresmywater.c`.

### Fixed this session (source git-tracked in this tree; see "commit" note at bottom)
1. **Water orientation (Stage 3) — FIXED via render-to-portrait-FBO.** Engine renders its whole
   frame into an offscreen 768×1024 FBO; platform presents it with ONE 90° rotated blit, retiring
   the fragile per-call glOrthof/viewport/scissor rotation hooks (which rotated the fluid's RTT pass
   inconsistently = the water bug). Code: `apkenv.h` (`ModuleHacks.{render_to_fbo,fbo_w,fbo_h}`,
   `apkenv_fbo_present` decl); `compat/gles_wrappers.c` (`apkenv_fbo_present`, fb-0→FBO redirect in
   `my_glBindFramebufferOES`); `platform/webos.c` (present before `SDL_GL_SwapBuffers`); module sets
   the flags + `current_orientation=LANDSCAPE`. Two bring-up bugs fixed: **viewport save/restore**
   (engine sets glViewport only at init → 70%/stretch), and **full GL-state save/restore around the
   blit** (else the attract screen's between-frame async skeleton load corrupts → intro hangs on
   `Screen_Credits`). Default ON; `APKENV_WMW_FBO=0` reverts to legacy hooks.
2. **In-level touch CADENCE — FIXED via frame-paced delivery.** A tap's DOWN+UP were landing on the
   SAME engine frame (`[WMWTOUCH] f1723 BEGAN+ENDED`), so the per-frame `WidgetManager::update`
   capture never ran between them (finger ABSENT from the map). Fix: `FingerPace` FSM in
   `modules/wheresmywater.c` dispatches ≤1 transition/finger/engine-frame (DOWN frame N, UP held to
   a later frame) from the GL thread before `rendererDrawFrame`. `APKENV_WMW_PACE`, default ON.
3. **In-level carve + HUD — root cause FOUND + fixed.** A **full-screen** (AABB `0,0..768,1024`),
   **key-0 `Widget_PushButton`** (vtable+8 = `0x469928`, the "press Swampy" widget; `handleEvent
   a=10`→`SwampyPressOnLevel`) swallows EVERY touch on EVERY level (it takes the single key-0 slot in
   `WidgetManager::update`'s Phase-2 entered-tree — keyed by `widget+0x10` set via
   `addWidget(w,key)`, lowest-key-first — ahead of the carve `FingerCatcher` key=5 AND the HUD
   buttons). Confirmed on level 2 (no tutorial). **Fix = kill-thief**: in `wm_hook_update`, when a
   full-screen key-0 PushButton captures, disable it (`+0x60=0`, skipped in the contains-walk) and
   release the finger so the carve/HUD get it. `APKENV_WMW_KILLTHIEF`, default ON. **Verified
   on-device: carve + HUD both work.**

### Disproven this session (don't re-test): coords/space, per-frame dt (WidgetManager::update's dt
arg is unused), touchMoved-catch, threading, async-load ORDER (widget keys are content-fixed:
FingerCatcher always 5, PushButton always 0).

### Open / next session (priority order)
- **Audio (Stage 4) — NOT STARTED. Highest-value next.** Implement `android.media.AudioTrack` in
  fake-JNI over a lock-free ring buffer drained by SDL audio, + reimplement FMOD's
  `org.fmod.FMODAudioDevice.run()` pump in C. Full design: `../plan/STAGE-4-audio.md`.
- **Precise thief fix (refinement, not blocking).** kill-thief is heuristic (vtable + full-screen
  check) and disables the minor tap-Swampy feature. Clean fix needs: why the button's bounds are
  full-screen here vs localized on Android (use **ACL** as an Android reference to diff), and/or why
  its `releaseFingerMoved` (returns 0) doesn't hand drags to the carve. The static logic says it
  should block Android too — unresolved without an Android reference.
- **Real accelerometer (deferred):** Luna bus / SDL joystick (`../plan/accelerometer-luna.md`).
- **Generalize to a 2nd game + `.ipk` packaging** (Stages 5, 6).
- **Cleanup:** kill-thief currently lives inside the debug `wm_hook_update`; a clean build moves it
  out and strips the (capped) `[WMWARB]`/`[WMWADD]`/`[WMWKILL]` logging.

### Env knobs (default = playable; set to 0 to disable): `APKENV_WMW_FBO`, `APKENV_WMW_PACE`,
`APKENV_WMW_KILLTHIEF`; `APKENV_WMW_FBO_ROT` (0-3, default 1=90°). Older debug knobs (HOOK,
FORCECATCH, MULTITOUCH, …) unchanged. Launchers: **play.sh** (now fully playable), play-fbo.sh,
play-kill.sh, stop.sh. Full staged plan + per-stage detail: `../plan/` (esp. STAGE-2/STAGE-3 §8).

### ⚠️ Source is git-tracked here but the session's changes are UNCOMMITTED. To lock against the
prior "source lost" incident: `git -C /home/jonwise/Projects/webos-android/apkenv add -A && git commit -m "WMW playable: FBO water + paced touch + kill-thief"`

---

This tree is the **reconstructed** apkenv webOS port for running the Android NDK
game **Where's My Water?** on the HP TouchPad without ACL. The original port
source was lost (it lived in an ephemeral session scratchpad). This is the
rebuild from upstream `thp/apkenv` + reverse-engineering notes. Keep it here in
the project, version it, never let it live only in scratch again.

## Layout (what's reconstructed vs. upstream)
- `platform/webos.c`        — NEW. webOS backend (PDL+SDL). See header comment.
- `platform/webos.mk`       — (not used; build is via build-webos.sh, see below)
- `modules/wheresmywater.c` — NEW. WMW per-game module (JNI entrypoints + touch).
- `apklib/apklib.c`         — PATCHED `apk_get_shared_libraries`: skip zip dir
                              entries ("lib/armeabi-v7a/") and `continue` (not
                              `break`) on fopen fail. Without this: "Not a native APK".
- `compat/pdk_compat.h`     — NEW. force-included; declares bsd_signal/__*_chk.
- `glshim/`                 — NEW. Khronos GLES/GLES2/EGL/KHR headers, copied so
                              `-nostdinc` doesn't pull host glibc 2.38 headers.
- `devlibs/libEGL.so`       — pulled from device /usr/lib (not in PalmPDK SDK).
- `build-webos.sh`          — NEW. the two-toolchain cross-build (see below).
- everything else           — pristine upstream thp/apkenv.

## Toolchain (host)
- COMPILE: `arm-linux-gnueabi-gcc-13` forced onto PalmPDK glibc-2.4 headers
  (`-nostdinc -isystem <gcc13 builtins> -isystem /opt/PalmPDK/arm-gcc/sysroot/usr/include`).
  gcc-13 tolerates the duplicate GLES1/GLES2 typedefs; forcing old headers
  avoids `__isoc23_*` / GLIBC_2.38 refs the device lacks.
- LINK: PalmPDK `/opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-gcc` (4.3.3) so
  symbols bind to glibc 2.4. Device GLES1 lib is the OLD name `libGLES_CM.so`.
- `-fgnu89-inline` (old-glibc extern-inline dup-symbol), `-fno-builtin
  -fno-stack-protector`, `-include compat/pdk_compat.h`.
- Do NOT `-DGLchar=char`: the modern glshim glext.h already typedefs GLchar.
- `-DAPKENV_LOCAL_BIONIC_PATH="./libs/webos/"` — where the linker finds the
  harvested bionic libs (libc/libstdc++/libm/liblog/libz).

## Build / deploy / run
    ./build-webos.sh                 # -> ./apkenv (ARM, interp ld-linux.so.3, GLIBC_2.4)
    # deploy (scp's legacy proto fails; pipe over ssh cat):
    ssh <legacy-opts> root@192.168.10.88 'cat > /var/apkenv/apkenv.new && chmod +x ... && mv ...' < apkenv
    # run on device:
    sh /var/apkenv/play.sh      # LEGACY single-finger touch (menus work)
    sh /var/apkenv/play-mt.sh   # MULTI-touch mode (APKENV_WMW_MULTITOUCH=1) for in-game swipe
    sh /var/apkenv/stop.sh
    # log: /tmp/wmw.log   (our touch instrumentation lines are "[WMWTOUCH] ...")

SSH (legacy ciphers): `ssh -oKexAlgorithms=+diffie-hellman-group1-sha1
-oCiphers=+aes128-cbc,3des-cbc -oHostKeyAlgorithms=+ssh-rsa,ssh-dss
-oPubkeyAcceptedAlgorithms=+ssh-rsa,ssh-dss -oMACs=+hmac-sha1 -i ~/.ssh/id_rsa
root@192.168.10.88`

Device layout: apk + asset root on `/media/internal` (big, noexec); exec
artifacts (apkenv + libs/webos + extracted .so) on `/var/apkenv` (ext3, exec ok).
The apk `/media/internal/wheresmywater.apk` has the libwmw std::string 1-insn
patch baked in (md5 cdf477f1...). gameroot assets at `/media/internal/gameroot`.

## WMW JNI entrypoints (confirmed: readelf + disassembly)
- `WMWRenderer_rendererInit(String sourceDir=apkPath, String dataDir, Context)`
- `WMWRenderer_rendererResized(int w, int h)`, `notifyScreenResized` (no-op)
- `WMWRenderer_rendererDrawFrame()` (logic + render; no separate update)
- `rendererTouchBegan/Ended(int n, float[] xs, float[] ys, int[] ids)`  (2 float + 1 int arrays)
- `rendererTouchMoved(int n, float[] xs, float[] ys, float[] prevXs, float[] prevYs, int[] ids)` (4 float + 1 int arrays)
- `BaseActivity_{onGamePause,onGameResume,onLostFocus,onRegainedFocus,accelerometerChanged(f,f,f),backKeyPressed}`

## Touch facts (hard-won)
- PDL_Init BEFORE SDL_Init; PDL_SetTouchAggression(MORETOUCHES); PDL_GesturesEnable(FALSE).
- Engine wants NORMALIZED 0..1 coords (WMWView.copyTouches: getX/getWidth).
- Portrait render on landscape FB via apkenv GLES1 hooks: current_orientation=
  PORTRAIT, gles_viewport_hack=1, glOrthof_rotation_hack=1; platform get_orientation=LANDSCAPE.
- Coord transform (1024x768 landscape surface, inverting the 90° render rot):
  `fx=(H-rawY)/H ; fy=rawX/W`.

## CURRENT STATE (this rebuild)
- BUILT, deployed, BOOTS clean, renders, 0 crashes. `webos_init: surface
  1024x768 gles=1`, `wheresmywater_init ... touch-mode=legacy`.
- Menus expected interactive (legacy path reproduces the prior working build).
- OPEN: in-game touch / swipe (Screen_WaterTest). Hypothesis: needs real
  MULTI-touch (carve/swipe). `play-mt.sh` enables it; `[WMWTOUCH]` lines log
  every dispatch (mode/finger-ids/normalized coords) to correlate with the
  engine's own `ScreenEnter:Screen_WaterTest` analytics lines.

## IN-GAME TOUCH INVESTIGATION (2026-06-27, in progress)
Symptom: menus fully work; in a level (Screen_WaterTest) NOTHING responds to
tap/swipe (carve AND HUD/pause). A webOS task-switch out+back makes touch work
(pause menu tappable). Game renders/animates fine in-level.

Verified (libwmw is NOT stripped — full C++ symbols; static RE + runtime probes):
- Our shim DELIVERS touches: rendererTouch* enqueue MotionEvent; rendererDrawFrame
  -> pumpEvents() drains every frame -> ApplicationContext::touch* ->
  ScreenManager::touchMoved. Touch gate ScreenManager::mCurrentTransition is OPEN
  (nil) in-level. Active screen IS Screen_WaterTest (stack probe: screens=1,
  top vtable 0x46b4b8). Dispatcher calls screen vtable+0x2c = touchMoved (NOT
  +0x40 = consumesInput). **Inline-hooked Screen_WaterTest::touchDown/touchMoved
  — THEY FIRE in-level.** So the handler runs; failure is BELOW it.
- Screen_WaterTest::touchMoved/touchDown forward to this->WidgetManager (this+0x50)
  -> WidgetManager::touchMoved(0x1ea99c)/touchDown(0x1edc10). Both gate on a byte
  at WidgetManager+0x60 (if 0, skip all hit-test/dispatch). BUT the WidgetManager
  ctor inits +0x60 = 1, and nothing writes 0 to the *manager's* own +0x60 (the
  regainedTop/_finalLoadStep writes are to sub-widgets via getWidget(id)+0x60).
  So the master gate is ~certainly 1 in-level => not the cause.
- Walaber::Vector2 is passed BY POINTER (r2/r3 = addresses); first hook misread
  it by-value -> garbage coords (fixed now to deref).
- regainedTop() (runs when screen returns to top after pause is popped) ENABLES
  sub-widgets (getWidget(7/8)+0x60=1) — explains why task-switch restores touch.
- Carve = WaterConcept::World::handleTouch* (0x2c3c38), called INDIRECTLY
  (registered receiver), not from Screen_WaterTest::touchMoved.

LEADING REMAINING HYPOTHESES (need runtime coords to decide):
  (A) coordinate-SPACE mismatch: our touches reach WidgetManager but the hit-test
      (AABB::contains) misses because level widgets/carve live in a different
      space (world/camera, or the portrait 768x1024 vs our normalized/rotated
      coords) than menu widgets. Menus work because menu widgets are in the space
      our coords map to. NOTE: user previously ruled out coords for the carve
      ("nothing anywhere") — but a degenerate/rotated mapping also yields nothing
      anywhere, so re-examine via the REAL coords the hook now logs.
  (B) per-widget enables: the carve surface / HUD button widgets have +0x60=0.

NEXT TEST (needs device + a level): run play.sh, enter a level, swipe. Read:
  - [WMWHOOK] touchDown/touchMoved lines: REAL coords + wm gate value.
  - [WMWSTACK] ... gate=N : the manager gate value in-level.
Then: if gate==1 and coords sane -> chase hit-test/coordinate-space (dump a
level widget's AABB vs our coords) or per-widget enables. play-force.sh
(APKENV_WMW_FORCEINPUT=1) force-sets the manager gate=1 as a control test.

## ✅ IN-GAME TOUCH — FIXED (2026-06-27). Level completed, 3 stars.

FIX = FORCE-CATCH (modules/wheresmywater.c, env APKENV_WMW_FORCECATCH=1,
launcher /var/apkenv/play-catch.sh). Each frame in Screen_WaterTest: if finger
id 0 is present in the WidgetManager(screen+0x50) finger-map and not already
forced this gesture, call Walaber::Widget_FingerCatcher::_acceptFinger(catcher,
0, FingerInfo) directly. catcher ptr = captured from Screen_WaterTest::handleEvent's
3rd arg (widget) when its vtable == FingerCatcher; _acceptFinger @link 0x1fe428
+ slide; finger-map = std::map<int,FingerInfo*> at wm+0x28 (node key@16 val@20),
walked for id 0. Confirmed: World::handleTouchDown fires (~921/level),
handleEvent count[+12]=1, LevelEvent EventType:Win Stars:3.

WHY it was needed: the slim shim sends one BEGAN/finger, so WidgetManager::touchDown
ALWAYS takes the create path (never the hit-test/catch path — confirmed by probe:
every touchDown = NOT-FOUND->CREATE), and the per-frame enter-detection never
caught our finger either (catcher +0x60 was already 1; setting +0xe0 didn't help).
So the catcher reported 0 caught touches -> carve loop bailed. Calling _acceptFinger
ourselves replicates the catch the engine couldn't do for our touch lifecycle.

REMAINING: HUD pause/reset BUTTONS may still not capture (separate widgets, not the
FingerCatcher; level is completable via carve so low priority). TODO: make
force-catch the DEFAULT (drop the env gate) + strip the debug hooks/logging for a
clean build so plain play.sh works; then sound (FMOD) + .ipk packaging.

## (history) IN-GAME TOUCH — ROOT CAUSE LOCALIZED (2026-06-27, before the fix)

Bug precisely localized via libwmw static RE (full C++ symbols) + runtime inline
hooks (harness in modules/wheresmywater.c: wmw_install_hook, env APKENV_WMW_HOOK).

VERIFIED touch pipeline (all correct):
  finger -> rendererTouch* (enqueue ndk::MotionEvent) -> rendererDrawFrame ->
  pumpEvents() (drains every frame) -> ApplicationContext::touch* ->
  ScreenManager::touchMoved (gate mCurrentTransition=nil=open) -> active screen
  = Screen_WaterTest (confirmed via stack probe) -> Screen_WaterTest::touch*
  (HOOKED, FIRE in-level) -> WidgetManager::touch* (this+0x50, manager gate
  +0x60=1=enabled). Real coords are SANE portrait px (768x1024); button taps HIT
  the correct button AABBs (valid=1). So delivery, coords, hit-test all OK.

THE BUG: the gameplay widget = **Walaber::Widget_FingerCatcher** (full-screen,
infinite-bounds AABB). It fires Screen_WaterTest::handleEvent(a=5) every frame,
but with WidgetActionRet+12 == 0 (touch-point count). handleEvent's a=5 path:
`if (count<=0) -> bail (no carve)`; count>0 -> _screenToWorld + World::handleTouch*
(the carve). Count comes from Widget_FingerCatcher::update -> [catcher+0xbc]
(caught-finger count) = 0. So the catcher NEVER CATCHES our finger ->
World::handleTouch{Down,Moved,Up} fire 0 times -> no carve. Buttons similarly
never capture (handleEvent only ever fires for the FingerCatcher, never buttons).

WHY no catch (the core finding): Widget_FingerCatcher catches a finger only via
  - acceptNewFingerDown (vtable+0x3c, unconditional -> _acceptFinger), called from
    WidgetManager::touchDown's HIT-TEST path (0x1ede08); OR
  - acceptNewFingerEntered (vtable+0x40), gated on catcher[+0xe0], called from
    WidgetManager::update's per-frame enter-detection (0x1ed0e0 region).
  BUT: (1) WidgetManager::touchDown for a FRESH finger id takes the CREATE path
  (allocates FingerInfo, returns) and NEVER reaches the hit-test branch (confirmed:
  create path has no branch to 0x1ede08). The hit-test only runs when the finger
  id ALREADY EXISTS in the FingerInfo map (wm+0x28 std::map<int,FingerInfo*>) with
  FingerInfo+20(captured-widget)==0. We send one BEGAN per finger -> always fresh
  -> always create -> never hit-test -> acceptNewFingerDown never called
  (_acceptFinger fires 0x). (2) enter-detection never fires acceptNewFingerEntered
  for our finger because the finger is INSIDE the infinite-bounds catcher from
  creation (no outside->inside transition); setting catcher[+0xe0]=1 did NOT help
  (acceptNewFingerEntered still not called).

EXPERIMENTS TRIED (all NEGATIVE, env-gated launchers on device /var/apkenv/):
  - multi-touch (play-mt.sh, APKENV_WMW_MULTITOUCH): n=1 single finger anyway.
  - UI/GL thread split (APKENV_WMW_UITHREAD, default on): no change.
  - call regainedTop() on entry (play-regain.sh, APKENV_WMW_CALLREGAIN): fired
    cleanly, no change (only enables sub-widgets; not the catch).
  - force manager gate (play-force.sh, APKENV_WMW_FORCEINPUT): moot (gate already 1).
  - double BEGAN (play-dd.sh, APKENV_WMW_DOUBLEDOWN): no change — engine likely
    dedups a 2nd down for an already-down finger, so the 2nd touchDown that would
    hit-test never happens.
  - enter-catch (play-ec.sh, APKENV_WMW_ENTERCATCH, set catcher+0xe0=1): no change
    (acceptNewFingerEntered not called; no enter transition for inf-bounds catcher).

OPEN PARADOX / NEXT IDEAS (need investigation; some need device tests):
  - How does Android EVER capture on a fresh down if create-path never hit-tests?
    Suspect: the FingerInfo pre-exists (created by something before touchDown), OR
    the engine's MotionEvent action sequence differs. RE-READ the decompiled
    WMWView.copyTouches (baksmali classes.dex) + verify what ndk MotionEvent action
    rendererTouchBegan sets vs what pumpEvents expects (0=began/1=moved/2=ended).
  - Verify FingerInfo+20 writers across the binary (what actually captures).
  - Possible fix: bake a libwmw patch so WidgetManager::touchDown's create path
    ALSO hit-tests (capture on fresh down). Risky but direct.
  - Re-examine whether our BEGAN reaches WidgetManager::touchDown's hit-test at all
    (hook the 0x1edef0 contains call-site vs 0x1ed0e0 to distinguish touchDown vs
    update-loop contains; the 48/133 HITs may all be from the update enter-loop).

Key libwmw offsets (link-time; runtime = +slide, slide=0x2c118000 last run):
  ScreenManager::touchMoved 0x274104; mScreenStack 0x470e78; mCurrentTransition
  0x470ec0; Screen_WaterTest vtable-ptr 0x46b4b8, touchDown 0x3aea8c, touchMoved
  0x3aec0c, touchUp 0x3aeabc, handleEvent 0x3b0454, regainedTop 0x3ac2ec;
  WidgetManager touchDown 0x1edc10, touchMoved 0x1ea99c, touchUp 0x1ee168,
  update 0x1ece38 (acceptNewFingerEntered calls @0x1ed1ec/0x1ed6ec), ctor sets
  own +0x60=1; AABB::contains 0x25d168 (layout min@0/4 max@8/12 valid@16);
  Widget_FingerCatcher vtable-ptr 0x469618, _acceptFinger 0x1fe428,
  acceptNewFingerDown 0x1fe7bc, acceptNewFingerEntered 0x1fe7a4 (gate +0xe0),
  update 0x1fdf88 (count from catcher+0xbc); World handleTouchDown 0x2c3c84,
  handleTouchMoved 0x2c3c38, handleTouchUp 0x2c3bf4.
Saved log: apkenv/last-wmw.log. Extracted lib: scratchpad wmwlib (per session).

DELIVERY IS FAITHFUL TO ANDROID (confirmed 2026-06-27 via baksmali classes.dex,
com/disney/common/WMWView.smali). onTouchEvent packed-switch: ACTION_DOWN(0)->
copyTouches->rendererTouchBegan(count,xs,ys,ids); MOVE(2)->rendererTouchMoved
(+xLast,yLast); UP(1)->rendererTouchEnded; POINTER_DOWN/UP(5/6)->copyTouch single.
copyTouches: _xPos=getX/getWidth, _yPos=getY/getHeight (normalized 0..1),
_ids=getPointerId (0 for 1st finger), last-pos from getHistoricalX/Y. This is
EXACTLY our shim's delivery (one Began/finger, normalized coords, id 0, last-pos
on moves). => The carve failure is NOT in the touch feed; it's an engine-STATE
difference from running the native engine WITHOUT its Java/Activity lifecycle.
The FingerCatcher's capture precondition (whatever Android satisfies via full
app/level init) isn't met. NEXT decisive probe (needs device): hook
acceptNewFingerDown(0x1fe7bc) and acceptNewFingerEntered(0x1fe7a4) separately to
see which catch path Android-vs-us attempts; and hook the WidgetManager::update
enter-loop contains call-site (0x1ed0e0) to see if the FingerCatcher is even
processed there (passes the +0x60/layer checks). Candidate fix: bake a libwmw
patch to capture-on-fresh-down, OR find+set the missing "level active" state.

## Touch-mode switch (no rebuild)
`APKENV_WMW_MULTITOUCH=1` -> module aggregates ALL active fingers into the JNI
arrays with real finger ids (for swipe/carve). Unset/0 -> legacy single pinned
finger id 0, count=1 (the menu-working path). Implemented in modules/wheresmywater.c.
