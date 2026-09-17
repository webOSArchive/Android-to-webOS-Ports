#!/bin/bash
# tr2-run.sh [<logname>] - one full test cycle for ANY packaged port (the name is
# historical: it was written for Temple Run 2). Set APPID for other games:
#   kill any running instance -> palm-install the newest .ipk -> palm-launch ->
#   wait -> pull /media/internal/apkenv-<appid>.log into plan/logs/<logname>.
#
# Why the kill matters (cost me two confusing runs): palm-launch on an app that
# is ALREADY RUNNING just brings its card to the front. The old process keeps
# writing the same log file, so you pull a log produced by the PREVIOUS build
# while `md5sum` on the installed binary says the new one is there. `killall`
# via novacom reports "unexpected EOF from server" and does not always take -
# `pidof` + `kill -9` does.
#
# Other novacom facts this encodes:
#   - the "--" separator belongs in nc_run ONLY; a second one at the call site
#     is passed through as a literal argv[1].
#   - `novacom run /bin/sleep -- N` is the way to wait; the host-side shell
#     cannot sleep in this harness.
#   - a wedged host daemon needs: sudo systemctl restart novacomd
set -e
cd "$(dirname "$0")/.."
APPID=${APPID:-com.apkenv.templerun2}   # e.g. APPID=com.apkenv.aralon for another Unity port
LOGNAME="${1:-tr2-run}"
WAIT="${WAIT:-30}"
OUT=../plan/logs/$LOGNAME.log
mkdir -p ../plan/logs

nc_run() { timeout --foreground 300 novacom run "file://$1" -- "${@:2}" < /dev/null 2>&1 || true; }

IPK=$(ls -t packaging/out/${APPID}_*.ipk | head -1)
echo "== package: $IPK"

PID=$(nc_run /bin/pidof apkenv | tr -dc '0-9 ' | awk '{print $1}')
if [ -n "$PID" ]; then
    echo "== killing running apkenv (pid $PID)"
    nc_run /bin/kill -9 "$PID" >/dev/null
    nc_run /bin/sleep 5 >/dev/null
fi

# A big package (Aralon: 291 MB with its OBB) spends ~5 min in the USB copy and
# several more while the device gunzips it at ~1 MB/s. A timeout that fires
# mid-unpack would launch a half-installed app and pull a misleading log.
echo "== installing"
timeout --foreground "${INSTALL_TIMEOUT:-1800}" palm-install "$IPK" 2>&1 | tail -1

echo "== launching"
timeout --foreground 120 palm-launch $APPID 2>&1 | tail -1
echo "== waiting ${WAIT}s"
nc_run /bin/sleep "$WAIT" >/dev/null

echo "== pulling log -> $OUT"
timeout --foreground 120 novacom get "file:///media/internal/apkenv-$APPID.log" > "$OUT" < /dev/null 2>/dev/null
echo "== $(wc -l < "$OUT") lines"
grep -anE "EGLWARM|EGLSHIM\] eglCreateContext ->|GLES.*table:|webos_init:|GLSL|\[UN-TOUCH\]" "$OUT" | head -20
