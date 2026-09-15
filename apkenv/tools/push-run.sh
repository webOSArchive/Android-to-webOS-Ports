#!/bin/bash
# push-run.sh [<logname>] - binary-only test cycle for an already-installed
# apkenv package. Use this for every iteration that changes only the apkenv
# binary; rebuild the .ipk (tools/tr2-run.sh) only when the env file, the data
# tree or the apk changes. A 50-290 MB package costs minutes per install.
#
#   APPID=com.apkenv.fruitninja tools/push-run.sh fn-02
#
# Encodes the traps from PORTING-PLAYBOOK.md §5:
#   - palm-launch on an already-running app only raises its card, and the old
#     process keeps writing the same log; kill by pidof + kill -9 first
#     (killall reports "unexpected EOF from server" and does not always take).
#   - `novacom put` over a binary a live process still holds fails; if the
#     script carried on it would launch the OLD binary with the NEW settings.
#     So: refuse to launch unless the md5 on the device matches this build.
#   - run every device command under `timeout`; a hung novacom run wedges the
#     host daemon (recover with: sudo systemctl restart novacomd).
#   - `kill -9` leaves the app's PDK jail bind-mounts behind (webOS only tears
#     them down on a clean exit). They accumulate across iterations — after ~15
#     cycles the device had 22 of them and /media/internal had come unmounted,
#     which makes palm-install fail with `file open failed` on
#     /media/internal/.developer. Check `grep -c palm/jail /proc/mounts` and
#     whether store-media is mounted before blaming the package; a reboot
#     clears both.
set -e
cd "$(dirname "$0")/.."
APPID=${APPID:?set APPID, e.g. APPID=com.apkenv.fruitninja}
LOGNAME="${1:-push-run}"
WAIT="${WAIT:-45}"
APPDIR=/media/cryptofs/apps/usr/palm/applications/$APPID
OUT=../plan/logs/$LOGNAME.log

nc_run() { timeout 300 novacom run "file://$1" -- "${@:2}" 2>&1 || true; }

[ -f apkenv ] || { echo "build the binary first: ./build-webos.sh"; exit 1; }
LOCAL_MD5=$(md5sum apkenv | cut -d' ' -f1)
echo "== local apkenv md5 $LOCAL_MD5"

for _ in 1 2 3 4 5; do
    PID=$(nc_run /bin/pidof apkenv | tr -dc '0-9 ' | awk '{print $1}')
    [ -z "$PID" ] && break
    echo "== killing running apkenv (pid $PID)"
    nc_run /bin/kill -9 "$PID" >/dev/null
    nc_run /bin/sleep 3 >/dev/null
done
PID=$(nc_run /bin/pidof apkenv | tr -dc '0-9 ' | awk '{print $1}')
[ -z "$PID" ] || { echo "apkenv still running (pid $PID) — not pushing over a live binary"; exit 1; }

echo "== pushing apkenv -> $APPDIR/apkenv"
timeout 300 novacom put "file://$APPDIR/apkenv" < apkenv

DEV_MD5=$(nc_run /usr/bin/md5sum "$APPDIR/apkenv" | tr -dc '0-9a-f \n' | awk '{print $1}' | head -1)
echo "== device apkenv md5 $DEV_MD5"
[ "$DEV_MD5" = "$LOCAL_MD5" ] || { echo "MD5 MISMATCH — refusing to launch a stale binary"; exit 1; }
nc_run /bin/chmod 755 "$APPDIR/apkenv" >/dev/null

echo "== launching"
timeout 120 palm-launch "$APPID" 2>&1 | tail -1
echo "== waiting ${WAIT}s"
nc_run /bin/sleep "$WAIT" >/dev/null

echo "== pulling log -> $OUT"
mkdir -p ../plan/logs
timeout 180 novacom get "file:///media/internal/apkenv-$APPID.log" > "$OUT" 2>/dev/null
echo "== $(wc -l < "$OUT") lines"
