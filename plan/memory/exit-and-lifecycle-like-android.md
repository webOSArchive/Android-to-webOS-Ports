---
name: exit-and-lifecycle-like-android
description: Android never runs a game's C++ static destructors or a second onPause — exit with _exit after host cleanup; test pause/resume/quit headlessly
metadata:
  type: feedback
---
Swiping a *paused* Tiny Death Star card away aborted in `free(): invalid pointer`: glibc `exit()` ran the engine's lazily registered C++ static destructors, which double-freed FMOD state the game's background teardown had already released. Android kills the process after onDestroy, so those destructors never run. Fix in modules/cocos2dx.c: deinit registers `atexit(_exit(0) handler)` — in deinit, because function-local statics register later and atexit is LIFO — and never pauses twice.

**Why:** the user saw it as "the app crashed on swipe" even though the save was fine; it would have shipped otherwise. Found only because the user read the log situation right.

**How to apply:** every new module: guard pause/resume with a paused flag, and exit like Android. Test the lifecycle without a person: `palm-launch com.palm.calculator` (APPACTIVE lost → pause), `palm-launch <appid>` (resume), `kill -15` (SDL_QUIT → deinit), for both running and paused states. Related: [[drive-the-port-without-a-person]], [[bionic-stdio-is-an-abi]].
