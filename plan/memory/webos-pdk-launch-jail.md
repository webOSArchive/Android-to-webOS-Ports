---
name: webos-pdk-launch-jail
description: "How LunaSysMgr actually launches a TouchPad PDK app (hybrid jail, argv, stdio, self-locate) — gotchas not in the webOS-MCP docs"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 701e3f0d-49ca-453f-8441-9ea0afb567bc
---

How the HP TouchPad (webOS 3.0.5) **LunaSysMgr launches a PDK app** — discovered empirically by catching the live pid (none of this is spelled out in the webOS-MCP `pdk`/`gotchas` resources, though `gotchas` confirms the jail exists). Applies to ANY native PDK `.ipk`, not just our apkenv/WMW spike. See [[wrapper-spike-progress]].

- **Hybrid jail.** The app runs chrooted under `/var/palm/jail/<appid>/` as **uid 5003 (jailuser)**, gid 5000, with `LD_PRELOAD=libpvrtc.so`, CWD/HOME = the app install dir. The jail has its **own `/proc`**; **even root cannot `ptrace`** a jailed process (`strace -p` → EPERM). The jail is **torn down on exit**, so post-mortem log/file reads fail — inspect LIVE via `/proc/<pid>/root/...` (that path is the jail's filesystem view from the host).
- **`/media/internal` is bind-mounted RW into the jail** (real files visible). So user/game data can live there; only the binary itself needs the exec app partition. (apkenv loads the apk's native `.so` via its own anon-RWX userspace linker, so those can sit on noexec `/media/internal`.)
- **`main` in appinfo.json must be the native ARM binary.** A shell-script `main` is NOT exec'd by the PDK launcher (silently nothing runs).
- **sysmgr passes the launch-parameters JSON as argv** — e.g. `apkenv "{ }"`. A binary that treats its last arg as a file path will choke. Detect a packaged launch and ignore/override argv.
- **A jailed PDK app's stdout/stderr go NOWHERE** — not `/var/log/messages` (contra the webOS-MCP `pdk` doc). Redirect to a file on `/media/internal` from inside the binary to get any logs.
- **`argv[0]` from the launcher is unreliable** (may be a bare name). Self-locate via `readlink("/proc/self/exe")` to chdir into the app dir for run-dir-relative lookups.
- **Launch from a script** for testing: `luna-send -n 1 palm://com.palm.applicationManager/launch '{"id":"<appid>"}'`. **`palm-install` refuses a same-or-lower version** — bump `appinfo.json` version to reinstall. `palm-install`/`palm-package` are in PalmSDK; they talk to the device over novacom-USB.
