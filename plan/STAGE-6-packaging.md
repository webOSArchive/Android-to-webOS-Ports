# Stage 6 — `.ipk` packaging

**Objective:** ship the runtime + a game as a proper installable webOS **PDK `.ipk`** that launches
from the TouchPad launcher — no shell scripts, no manual `scp`.
**Architecture ref:** `../android-runtime-architecture.md` Phase 6.
**Depends on:** Stage 5.
**Status:** ✅ DONE — WMW installs as a `.ipk` and launches from the TouchPad launcher icon, runs jailed, GL + audio working (2026-06-28).

## 1. The webOS packaging contract (the spec to honor)

- PDK app = ARM Linux binary packaged as `.ipk`, `appinfo.json` with `"type":"pdk"` + `"main":"<binary>"`.
- Tools: **`palm-package`** / **`palm-install`** live in **PalmSDK** (`/opt/PalmSDK/Current/bin/`), not
  PalmPDK (`/opt/PalmPDK/` = headers + device libs only).
- Runtime filesystem facts: app install dir
  `/media/cryptofs/apps/usr/palm/applications/<id>/` is **read-only at runtime**; `/media/internal/`
  is writable; **binaries must run from an exec partition** (`/var` or the app dir — `/media/internal`
  is noexec). stdout/stderr → `/var/log/messages`.
- 3-layer compositor: SDL owns the GL context (no raw EGL); PDL_Init before SDL (already done).

## 2. Current state (as-built)

- Deploy is manual: `scp` over legacy-cipher SSH; run via `/var/apkenv/play*.sh`. Exec artifacts on
  `/var/apkenv/` (small ext3, exec ok), big data (apk + asset root) on `/media/internal`. No `.ipk`.
- Stability patch baked into the apk; bionic libs harvested under `libs/webos/`.

## 3. Work items

1. `appinfo.json` (`type:pdk`, id, version, icon, `main`), an app-dir layout, and a launcher entry
   binary (or the apkenv binary directly as `main` with the game path compiled/configured in).
2. Decide the exec-partition story under packaging: the app dir is exec-capable, but space + the
   harvested bionic libs + the apk/asset root must be placed correctly (app dir read-only at runtime →
   writable data under `/media/internal/.apkenv/<pkg>`, as already modeled).
3. `palm-package webos/` → `.ipk`; install via `palm-install` or on-device `ipkg install`.
4. A built-in **asset-root default** so it runs with **no env vars** (the gdbserver/env lesson:
   extraction is skip-if-exists; asset root has a built-in default).
5. One clean launcher path (no `play-*.sh` zoo).

## 4. Verification gate (on-device, pass/fail)

- [ ] `.ipk` installs and the app **launches from the TouchPad launcher** (icon tap), with **no env
      vars and no shell scripts**.
- [ ] Game runs with full Stage 2/3/4 behavior (input, orientation, audio) from the packaged build.
- [ ] Writable data lands under `/media/internal/.apkenv/<pkg>`; no attempt to write the read-only app
      dir; logs visible in `/var/log/messages`.

## 5. Review checklist (for an external reviewer)

- [ ] Does it run with **zero** environment configuration (built-in asset-root default, skip-if-exists
      extraction)?
- [ ] Are binaries on an **exec** partition and data on a **writable** one, per the webOS fs contract?
- [ ] Is GL still via SDL (no raw EGL) under the packaged layout — no compositor flicker?
- [ ] Is licensing/redistribution of the harvested bionic libs + game apk acceptable for the intended
      distribution? (Flag; not a technical gate.)

## 6. Risks / open questions

- Harvested bionic libs + game apk redistribution is a legal question, not just packaging — note who
  the build is for.
- Memory: `requiredMemory` / appinfo tuning for the TouchPad may be needed (see PDK shine notes).

## 7. Work log

### 2026-06-28 — offline scaffolding + structure validation.

PalmSDK packaging tools confirmed present (`palm-package`/`palm-install`/
`palm-generate`). Built:
- `apkenv/packaging/webos/appinfo.json` — `type:pdk`, `main:apkenv`,
  id `com.apkenv.wheresmywater`, icon, `requiredMemory:64`.
- `apkenv/packaging/build-ipk.sh` — assembles the **runtime-only** .ipk (apkenv
  binary + harvested bionic libs + device libEGL + icon/appinfo) and runs
  `palm-package`. Extracts the launcher icon from the apk at package time (not
  committed — copyrighted). **Validated offline:** produces a structurally valid
  `.ipk` (debian-binary + control.tar.gz + data.tar.gz).
- `apkenv.c` + `build-webos.sh` — `APKENV_DEFAULT_APK` fallback so the binary,
  launched from the webOS launcher with no argv, finds the game apk.

**Design decision:** the .ipk ships the RUNTIME ONLY. The copyrighted game apk +
its extracted asset root stay on writable `/media/internal` (binary defaults to
`/media/internal/wheresmywater.apk`). Sidesteps both the .ipk size and the game
redistribution question, and matches the current on-device layout.

**Blocked on hardware (the verification gate + missing payload):**
1. The harvested gingerbread bionic libs (libc/libm/libstdc++/liblog/libz) live
   ONLY on the device (`/var/apkenv/libs/webos`), not in the repo — must be
   pulled into `packaging/staging-libs` before a runnable .ipk exists.
2. CWD assumption: apkenv loads bionic from `./libs/webos/` relative to CWD —
   confirm the launcher sets CWD = app install dir (else use an absolute
   `APKENV_LOCAL_BIONIC_PATH` or `chdir()` in `main()`).
3. App install dir must be exec-capable for the binary AND dlopen of the .so's.
4. **Asset-root open question:** the on-demand `fopen("/Textures/..")` redirect
   the device build relied on (per memory notes) is NOT present in this
   reconstructed `compat/libc_wrappers.c` (`my_fopen` is a plain passthrough) —
   resolve how assets actually load before trusting a no-env launch.
5. The real gate: installs + launches from the launcher icon with no env/scripts.

### 2026-06-28 (part 2) — DONE on device. The TouchPad PDK launch path, decoded.

`./packaging/build-ipk.sh` (after `scp`-ing the device bionic libs into
`packaging/staging-libs`) builds the `.ipk`; `palm-install` over novacom installs
it; it launches from the launcher icon. Verified: jailed app boots, GL on Adreno
220, FMOD audio (rate=24000), smooth render loop.

**Hard-won facts about how LunaSysMgr launches a PDK app (none of these are in
the webOS-MCP `pdk`/`gotchas` docs explicitly; found by catching the live pid):**
- **It runs in a hybrid jail** (`/var/palm/jail/<appid>/`) as **uid 5003
  (jailuser)**, `LD_PRELOAD=libpvrtc.so`, `HOME`/CWD = the app dir. The jail has
  its own `/proc`; **even root can't `ptrace`** it; the jail is **torn down on
  exit**, so post-mortem log reading fails — read the in-jail log live via
  `/proc/<pid>/root/media/internal/...`.
- **`/media/internal` IS bind-mounted rw into the jail** (real files visible), so
  the game apk + asset root can live there; only the binary needs the exec app
  partition (the apk's native libs load via apkenv's own userspace linker = anon
  RWX, fine on noexec media).
- **`main` must be the native ARM binary** — a shell-script `main` is NOT exec'd.
- **sysmgr passes the launch-params JSON as argv** (`apkenv "{ }"`). A naive
  apk-path parser treats it as the apk → "Not a native APK" → exit. Fix: on a
  packaged launch, ignore argv and force `APKENV_DEFAULT_APK`.
- **A jailed PDK app's stdout/stderr go nowhere** (not `/var/log/messages`).
  Redirect them to a file on `/media/internal` from inside the binary.
- **`argv[0]` from the launcher is unreliable** — self-locate via
  `/proc/self/exe` to chdir into the app dir (so `./libs/webos` resolves).

All four handled in `apkenv.c` under `#ifdef __webos__` (commit "Stage 6: WMW
launches from the webOS launcher").

### 2026-06-28 (part 3) — SELF-CONTAINED .ipk + device cleanup. DONE.

The `.ipk` now bundles the game apk, so a fresh device needs nothing pre-staged.
App-dir layout is organized so the shim's two halves are obvious to a future
reverse-engineer / re-packer:
```
apkenv  appinfo.json  icon.png  README
libs/webos/   webOS-side host libs apkenv loads (gingerbread bionic + libEGL)
android/      the Android NDK game .apk (engine .so + assets read from here)
```
- `apkenv.c` packaged launch auto-finds the game = first `*.apk` under
  `<appdir>/android/` (re-packing a different game = drop in its `.apk` + a
  matching module), path built from `/proc/self/exe`; falls back to
  `APKENV_DEFAULT_APK`.
- `packaging/webos/README` ships in the app dir explaining all this.
- Writable data still goes to `/media/internal/.apkenv/<apk-basename>/`
  (savegame + the apk's runtime-extracted native libs) and
  `/media/internal/apkenv-wmw.log`.

**Verified on device:** removed all cruft (`/var/apkenv`, `/media/internal/gameroot`,
the loose `/media/internal/wheresmywater.apk`), installed v1.0.1, launched from the
launcher → boots from the **bundled** `android/wheresmywater.apk`, GL on Adreno 220,
FMOD audio (rate=24000), nothing on `/media/internal` but the savegame dir. `.ipk`
is 24 MB. (Benign: the game logs `Database error: no such column: EventValue` from
its own analytics db — non-fatal.)

Notes: `palm-install` refuses a same/lower version → bump `appinfo.json`. Bundling
the apk is a game-redistribution call (fine for personal/archival use).

---

## 8. Future plan — external / bring-your-own-apk (BYO) distribution (NOT built yet)

**Why.** Bundling the game inside the `.ipk` (§7) is correct only for **free /
archival** content (e.g. WMW here). For a **paid or proprietary** game, the
freely-distributable artifact must contain only *our* runtime; the game `.apk`
must be supplied by the user who legally owns it — the **emulator-and-your-own-ROMs**
model (ship the emulator, not the games). The runtime is unchanged; only *where the
apk comes from* and *who ships it* change. We will not build this today, but the
design below is the target so today's choices don't paint us into a corner.

### 8.1 Principle
The distributed `.ipk` ("BYO build") ships: `apkenv` + `libs/webos/` + the
per-game **module** + `appinfo`/`icon`/`README`. It ships **no game bits** —
`android/` is empty (or absent). The user places their own `.apk` on the device;
the runtime finds and runs it. (The harvested gingerbread bionic libs in
`libs/webos/` are AOSP — BSD/Apache-2.0, redistributable — so they may ship; the
*game* may not. Keep that line bright.)

### 8.2 apk discovery — make the search a precedence list, not a single path
Today `apkenv.c` (webOS packaged launch) scans `<appdir>/android/*.apk`. Generalize
to an ordered search, first hit wins, logged:
  1. **explicit config** — `/media/internal/apkenv/<appid>/config` line `apk=/abs/path`
     (lets a user point at any location, incl. a USB-mounted file);
  2. **user drop dir** — first `*.apk` in `/media/internal/apkenv/<appid>/`
     (the conventional BYO location — on `/media/internal`, which is bind-mounted
     **rw into the jail**, so it's reachable and user-writable over USB/novacom);
  3. **bundled** — first `*.apk` in `<appdir>/android/` (the archival case, §7);
  4. **fallback** — `APKENV_DEFAULT_APK`.
This is purely additive — the archival build keeps working (hits #3).

### 8.3 Missing-apk UX (don't just exit)
If no apk is found, the binary must not silently die (a jailed PDK app's stdio is
invisible — see §7). Render a one-screen SDL message (the GL context is already up):
"Place <Game> .apk in /media/internal/apkenv/<appid>/ and relaunch." Optionally a
minimal SDL file picker over that dir. A companion Enyo/Mojo "setup" card is heavier
but friendlier; defer. Always also write the reason to
`/media/internal/apkenv-<appid>.log`.

### 8.4 Validate the supplied apk (fail clean, not crash)
The user may drop the wrong apk or wrong version. The per-game **module's
`match()`** should verify before driving it: package name + the engine `.so`
present (and ideally a known size/hash for versions we've mapped). On mismatch,
show §8.3-style guidance ("expected <pkg> v<x>"), don't run. This is also where
Stage 5's "facts, not behavior" module data pays off.

### 8.5 Runtime binary patching (keep patched game binaries out of the package)
WMW needed a 1-instruction `libwmw` fix; for archival we baked it into the bundled
apk. For BYO we must **not** ship a patched game binary and must **not** mutate the
user's original. Instead, **patch at first run into a writable working copy**:
on launch, if the engine `.so` extracted to `/media/internal/.apkenv/<basename>/lib/…`
matches a known-unpatched hash, apply the recorded patch (offset+bytes, carried as
*module data*) to that extracted copy only. The user's `.apk` stays untouched; the
distributed `.ipk` carries only a patch *description*, not game code.

### 8.6 Packaging flag + module strategy
- `build-ipk.sh`: add `--byo` (or `APK=` empty) → omit `android/*.apk`, write a
  BYO `README` (where to drop the apk), smaller `.ipk`.
- **One `.ipk` per game** (the game's module baked into `apkenv`, apk external) is
  the near-term shape — simplest, and each is independently distributable. A single
  multi-game runtime that bundles *several* modules and auto-selects by `match()`
  is a later option; the discovery + validation above already support it.

### 8.7 Open questions (resolve when we build this)
- Best user transfer path for the apk on a stock TouchPad (USB mass-storage to
  `/media/internal`, novacom, or an in-app fetch) — and the least-friction
  conventional dir name.
- Whether to support the apk on removable/USB storage (path via §8.2 #1 config).
- Legal copy: the README/about must state the user must own the game; no game
  bits, no patched game binaries in the package.
