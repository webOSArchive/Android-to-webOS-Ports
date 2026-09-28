#!/bin/bash
# sysgrab.sh [<name>] - screenshot the webOS panel from the workstation via
# LunaSysMgr, with nothing of ours running on the device.
#
# This is the on-device screenshot (the same one the power+center key combo
# takes): the compositor writes its composed scene as a PNG. It captures SDK
# apps (Mojo/Enyo cards), the launcher and the system UI. It does NOT capture
# the GL layer of a PDK or apkenv game - for a running apkenv game use grab.sh,
# which has the game read its own frame back.
#
# Output: packaging/out/screenshots/<name>-<timestamp>.png (gitignored), path
# printed on stdout. Override the directory with OUTDIR=.
#
# Mechanism: LunaSysMgr's private-bus method
#   palm://com.palm.systemmanager/takeScreenShot {"file": "<path>"}
# replies {"returnValue": true} once the file is written. Traps found on
# 2026-09-28: (1) the public bus (-P) refuses luna-send ("permissions does not
# allow inbound connections"); the private bus, which root on the device may
# use, works. (2) luna-send only waits for the reply in interactive mode
# (-i -n 1), and interactive mode exits on stdin EOF - with stdin at /dev/null
# it returned before the reply. So a backgrounded `sleep | luna-send` holds
# stdin open (the device busybox has no mkfifo), the reply goes to a file, and
# the helper returns as soon as that file is non-empty; the sleep expires on
# its own. (3) The call runs from a helper script pushed to the device because
# `novacom run --` mangles the quoting of the JSON argument.
set -e
cd "$(dirname "$0")/.."
NAME="${1:-sysgrab}"
OUTDIR="${OUTDIR:-packaging/out/screenshots}"
REMOTE=/tmp/apkenv-sysgrab.png
REMOTE_SH=/tmp/apkenv-sysgrab.sh
REMOTE_OUT=/tmp/apkenv-sysgrab.out

# Every novacom call runs under `timeout --foreground` with stdin at /dev/null
# (or a pipe): see grab.sh for the SIGTTIN hang this avoids. novacom reports a
# remote non-zero exit as "unexpected EOF from server".
nc_run() { timeout --foreground 60 novacom run "file://$1" -- "${@:2}" < /dev/null 2>&1 || true; }

if ! timeout --foreground 15 novacom -l < /dev/null 2>/dev/null | grep -q .; then
    echo "sysgrab: no device on novacom" >&2
    exit 1
fi

mkdir -p "$OUTDIR"
tmp=$(mktemp --suffix=.png)
trap 'rm -f "$tmp"' EXIT

# Host variables are expanded into the helper; \$ marks the device shell's own.
timeout --foreground 30 novacom put "file://$REMOTE_SH" <<DEV
#!/bin/sh
rm -f $REMOTE $REMOTE_OUT
sleep 30 < /dev/null 2>/dev/null | /usr/bin/luna-send -i -n 1 palm://com.palm.systemmanager/takeScreenShot '{"file":"$REMOTE"}' > $REMOTE_OUT 2>&1 &
i=0
while [ \$i -lt 20 ] && [ ! -s $REMOTE_OUT ]; do sleep 1; i=\$((i+1)); done
cat $REMOTE_OUT
rm -f $REMOTE_OUT
DEV

reply=$(nc_run /bin/sh "$REMOTE_SH")
if ! grep -q '"returnValue" *: *true' <<<"$reply"; then
    echo "sysgrab: takeScreenShot did not succeed: ${reply:-<no reply>}" >&2
    nc_run /bin/rm -f "$REMOTE_SH" >/dev/null
    exit 1
fi

if timeout --foreground 60 novacom get "file://$REMOTE" > "$tmp" < /dev/null 2>/dev/null && [ -s "$tmp" ]; then
    out="$OUTDIR/$NAME-$(date +%Y%m%d-%H%M%S).png"
    cp "$tmp" "$out"
    nc_run /bin/rm -f "$REMOTE" "$REMOTE_SH" >/dev/null
    echo "$out"
    exit 0
fi

nc_run /bin/rm -f "$REMOTE" "$REMOTE_SH" >/dev/null
echo "sysgrab: the service reported success but $REMOTE could not be fetched" >&2
exit 1
