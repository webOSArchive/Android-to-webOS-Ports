---
name: bionic-stdio-is-an-abi
description: bionic's fileno/feof/ferror are macros reading FILE fields; glibc FILEs handed to bionic code give fd 0 — empty files, silent failures
metadata:
  type: feedback
---
bionic `<stdio.h>` inlines `fileno`/`feof`/`ferror` as reads of the FILE struct (`_file` = short at offset 14). Engine code (e.g. a statically linked gnustl `std::ofstream`) never calls `fileno()`, so apkenv returning glibc `FILE*` from `fopen` made Tiny Death Star write its 11 MB FMOD banks to fd 0: files created, named right, 0 bytes, FMOD silently shut down.

**Why:** the open-only file trace said every open was "ok"; only a `writev` trace with fds exposed it. Cost 5 device runs (tds-03..08, 2026-09-16).

**How to apply:** when a file the engine writes comes out empty or garbage, trace write/writev fds first. Fix = `apkenv_bionic_stdio_enable()` in the module's try_init (bionic-layout FILE proxies in compat/libc_wrappers.c, opt-in so shipped ports are unchanged). Related: [[probe-every-path-the-engine-takes]], [[dont-touch-shipped-ports]].
