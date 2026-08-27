#!/bin/bash
# tr2-device-run.sh [c|d] - push and run the Temple Run 2 Mono checkpoints.
#
#   c   Checkpoint C: monotest - the native Mono runtime alone, no libunity,
#       no apkenv, no bionic code in the process. Proves JIT + Boehm GC +
#       Unity's corlib work on webOS.
#   d   Checkpoint D: apkenv with APKENV_HOST_MONO - the full bridge.
#
# Needs a TouchPad on USB with a responsive novacomd. If `novacom -l` hangs,
# the HOST daemon is wedged (a stuck `novacom run` session does it):
#     sudo systemctl restart novacomd
set -e
cd "$(dirname "$0")/.."
WHICH="${1:-c}"
DEV=/var/apkenv2
APK=../android-candidates/templerun2_1.2.1.apk
MANAGED_SRC=build/webos/tr2-managed

# NB: the "--" separator belongs HERE only - do not repeat it at call sites,
# or the target sees a literal "--" as its first argument (busybox then treats
# the real flags as operands, in a read-only cwd of "/").
nc_run() { timeout 300 novacom run "file://$1" -- "${@:2}"; }
# Always write to a temp name and mv into place: overwriting a binary that was
# just executed fails with "file open failed" (ETXTBSY) on the device.
nc_put() { timeout 300 novacom put "file://$2.new" < "$1" && \
           timeout 60 novacom run file:///bin/mv -- "$2.new" "$2"; }

[ -n "$(timeout 20 novacom -l 2>/dev/null)" ] || {
    echo "ERROR: no device (or novacomd is wedged: sudo systemctl restart novacomd)" >&2
    exit 1; }

# Managed assemblies, extracted from the PRISTINE apk (never modified).
if [ ! -d "$MANAGED_SRC" ]; then
    echo "EXTRACT Managed/ from $APK"
    mkdir -p "$MANAGED_SRC"
    ( cd "$MANAGED_SRC" && unzip -oq "$OLDPWD/$APK" 'assets/bin/Data/Managed/*' \
      && mv assets/bin/Data/Managed/* . && rm -rf assets )
fi

nc_run /bin/mkdir -p $DEV/managed

echo "PUSH libmono-webos.so"
nc_put hostlibs/webos/libmono-webos.so $DEV/libmono-webos.so
for f in "$MANAGED_SRC"/*.dll; do
    echo "PUSH $(basename "$f")"
    nc_put "$f" "$DEV/managed/$(basename "$f")"
done

if [ "$WHICH" = c ]; then
    echo "PUSH monotest"
    nc_put build/webos/monotest $DEV/monotest
    nc_run /bin/chmod 755 $DEV/monotest
    echo "=== RUN monotest ==="
    nc_run $DEV/monotest $DEV/libmono-webos.so $DEV/managed $DEV/etc
else
    echo "PUSH apkenv"
    nc_put apkenv $DEV/apkenv
    nc_run /bin/chmod 755 $DEV/apkenv
    echo "=== RUN apkenv with the Mono bridge ==="
    echo "(on device, run:)"
    echo "  cd $DEV && APKENV_HOST_MONO=$DEV/libmono-webos.so \\"
    echo "    MONO_LOG_LEVEL=debug MONO_LOG_MASK=asm,dll,gc \\"
    echo "    ./apkenv /media/internal/templerun2.apk 2>&1 | tee /media/internal/apkenv-tr2.log"
fi
