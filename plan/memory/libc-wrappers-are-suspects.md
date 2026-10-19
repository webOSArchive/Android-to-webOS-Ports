---
name: libc-wrappers-are-suspects
description: apkenv's hand-written libc wrappers can be wrong (strcasecmp prefix-matched, losing all PvZ saves); when a file op silently fails, strace the device and suspect the wrapper
metadata:
  type: feedback
---

PvZ HD (2026-10-19) never saved progress. The cause was apkenv's own `my_strcasecmp` (`compat/linux_wrappers.c`), which returned 0 for any prefix and for `""`. The engine resolved path components case-insensitively, so the empty component after its data root's trailing slash matched `_manifest.json`, and saves went to a path under a regular file (ENOTDIR, no error shown). It was found with device `strace -f -e trace=file` attached right after launch: repeated `mkdir`/`stat` on a nonsensical path.

**Why:** "saves don't persist" looks like a lifecycle or packaging problem. The real cause was a libc semantic bug in the shim that had been there since the first commit.

**How to apply:** for any "data not saved/found" symptom, first check that the save files exist on the device, then strace file syscalls and look for paths that make no sense. Treat every `my_*` wrapper in `compat/` as a suspect, and host-test a wrapper against glibc before trusting it. With two pads attached, set `DEVICE=<nduid>`: the device scripts source `tools/device.sh` and refuse to guess without it. Related: [[probe-every-path-the-engine-takes]], [[bionic-stdio-is-an-abi]].
