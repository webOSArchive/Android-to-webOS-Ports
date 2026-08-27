#!/bin/bash
# tr2-run.sh [<logname>] - one test cycle for the packaged Temple Run 2:
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
APPID=com.apkenv.templerun2
LOGNAME="${1:-tr2-run}"
WAIT="${WAIT:-30}"
OUT=../plan/logs/$LOGNAME.log

nc_run() { timeout 300 novacom run "file://$1" -- "${@:2}" 2>&1 || true; }

IPK=$(ls -t packaging/out/${APPID}_*.ipk | head -1)
echo "== package: $IPK"

PID=$(nc_run /bin/pidof apkenv | tr -dc '0-9 ' | awk '{print $1}')
if [ -n "$PID" ]; then
    echo "== killing running apkenv (pid $PID)"
    nc_run /bin/kill -9 "$PID" >/dev/null
    nc_run /bin/sleep 5 >/dev/null
fi

echo "== installing"
timeout 600 palm-install "$IPK" 2>&1 | tail -1

echo "== launching"
timeout 120 palm-launch $APPID 2>&1 | tail -1
echo "== waiting ${WAIT}s"
nc_run /bin/sleep "$WAIT" >/dev/null

echo "== pulling log -> $OUT"
timeout 120 novacom get "file:///media/internal/apkenv-$APPID.log" > "$OUT" 2>/dev/null
echo "== $(wc -l < "$OUT") lines"
grep -anE "EGLWARM|EGLSHIM\] eglCreateContext ->|GLES.*table:|webos_init:|GLSL|\[UN-TOUCH\]" "$OUT" | head -20
