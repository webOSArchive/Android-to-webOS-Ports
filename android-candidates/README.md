# Donor APKs go here

This folder is where you place the **Android `.apk`** you want to port — your
"donor" game. APKs are **never committed** (`.gitignore` excludes `*.apk`): this
is a bring-your-own-apk toolkit. Supply a game **you own / are entitled to use**.

```
android-candidates/
  your-game.apk        <- drop it here (git-ignored)
```

What makes a good Tier-2 (NDK-wrapper) candidate — the kind this toolkit targets:

- **Native NDK engine, no/minimal Dalvik** — the game logic lives in
  `lib/armeabi-v7a/*.so` driven through a few JNI entry points, not in Java.
- **GLES1 or GLES2** rendering (the TouchPad has both via `libGLES_CM` / `libGLESv2`).
- **Few outbound JNI classes** to stub (analytics, billing — fake them).
- Older titles (≈2011–2013, gingerbread-era) port most cleanly.

The worked example in this repo is **Where's My Water?** (Walaber engine, GLES1,
FMOD audio). See the field guide `../android-port-shim.md` and the candidate
ranking notes in `../CLAUDE.md` for how to triage an apk before committing to a
port, and `../plan/` for the staged porting methodology.

> Some games need a one-line binary patch to their engine `.so` (e.g. WMW's
> `std::string(NULL)` fix). Keep the original apk pristine; apply patches to a
> working copy (see the porting docs), and never commit the patched game binary.
