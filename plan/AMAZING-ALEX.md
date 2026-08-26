# Amazing Alex HD (Rovio ka3d) — ported in one pass (2026-08-26)

Third port; first one done entirely by the `PORTING-PLAYBOOK.md` method — booted, rendered, took
touch and played music on the **first device launch**. Elapsed: ~1 hour, no brute force.

## Triage (no device)
- `lib/armeabi/libamazingalex.so` (2.7 MB) = Rovio **ka3d** engine, the Angry Birds engine →
  apkenv's `modules/angrybirds.c` already drives it (`Java_com_rovio_ka3d_MyRenderer_native*`,
  `AudioOutput_nativeMixData`). GLES1, **landscape** (TouchPad-native), a `Data/1024X768` asset
  tier (exact match), 144 KB dex (Java is just the host + Flurry/billing glue).
- Host contract (`baksmali`): 16 natives, ~20 engine→Java call-outs. Differences vs the module:
  1. **`readFile(name)` appends `.zip` and returns the first zip entry inflated** (falls back to
     the raw asset). 9 big `.pvr.zip` sheets — a raw read would have fed the engine zip bytes.
  2. `nativeKeyInput(III)` (keycode, unicode, down) — module typedef had 2 args.
  3. Java calls `nativeInit(w,h,filesDir)` **then `nativeResize(w,h)`** — module skipped resize.
  4. `getUniqueId`/`getUniqueIdHash` → return `""`; `isSilentProfile` → false.
  5. `AudioOutput(J,I,I,I,I)` = (handle, freq, channels, bits, bufsize) — matches the module's
     `NewObjectV` reader; engine asked for 16 kHz stereo.
- webOS build didn't include `angrybirds.c` (`build-webos.sh` SOURCES) — added.

## Built
`modules/angrybirds.c`: zip-aware `readFile` (`ab_unzip_first_entry`, zlib raw inflate of the
local-file-header entry), `[AB-JNI] UNHANDLED` tracer, 3-arg key input, post-init resize,
`[AB]`/`[AB-FILE]`/`[AB-AUDIO]` log lines. Binary md5 `e5abce55` (also still runs PvZ/WMW).

## Result
First run log: `nativeInit(1024,768) -> 1`, `nativeResize -> 1`, all assets from `Data/1024X768`,
`AudioOutput(freq=16000 ch=2)` + `startOutput`, menu scenes loaded. Only unhandled call-outs:
`HashMap.put`, `toString` (harmless). User: "running, music playing, I'm in the game".
**Launcher-icon launch CONFIRMED** (user). Package: `packaging/out/com.apkenv.amazingalex_1.0.0_all.ipk` (23 MB, apk bundled; no data
seeding needed — the engine reads everything through `readFile` from the apk).
Harness: `/var/apkenv2/play-alex.sh`, apk `/media/internal/amazingalex.apk`, log
`/media/internal/apkenv-alex.log`.

## Open / to watch
- `WebViewWrapper` call-outs (news/ads screens) are unimplemented — if a screen hangs, the tracer
  will name the call.
- Flurry/billing Java is dead code here (no Dalvik) — fine unless the engine polls it.
