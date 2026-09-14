#!/bin/bash
# grab.sh [<name>] - screenshot the running apkenv game from the workstation.
#
# The on-device screenshot (and /dev/fb0) misses the GL layer: the webOS
# compositor owns the panel. So the game reads its own frame back instead
# (platform/webos.c, GRAB_*): this script drops the trigger file, the game
# writes its next frame to /media/internal/apkenv-grab.ppm within ~0.5 s, and
# the frame is pulled and saved as a PNG under packaging/out/screenshots/
# (gitignored - screenshots are game art).
#
# Needs a binary built with the grab trigger. Against an older one the trigger
# is never consumed; the script says so and removes it.
set -e
cd "$(dirname "$0")/.."
NAME="${1:-grab}"
OUTDIR="${OUTDIR:-packaging/out/screenshots}"
TRIGGER=/media/internal/.apkenv/grab
REMOTE=/media/internal/apkenv-grab.ppm

# novacom reports a remote non-zero exit as "unexpected EOF from server".
nc_run() { timeout 60 novacom run "file://$1" -- "${@:2}" 2>&1 || true; }

if [ -z "$(nc_run /bin/pidof apkenv | tr -dc '0-9')" ]; then
    echo "grab: no apkenv running on the device" >&2
    exit 1
fi

mkdir -p "$OUTDIR"
tmp=$(mktemp --suffix=.ppm)
trap 'rm -f "$tmp"' EXIT

nc_run /bin/rm -f "$REMOTE" >/dev/null
timeout 30 novacom put "file://$TRIGGER" < /dev/null

for _ in $(seq 1 15); do
    if timeout 60 novacom get "file://$REMOTE" > "$tmp" 2>/dev/null && [ -s "$tmp" ]; then
        out="$OUTDIR/$NAME-$(date +%Y%m%d-%H%M%S).png"
        convert "$tmp" "$out"
        nc_run /bin/rm -f "$REMOTE" >/dev/null
        echo "$out"
        exit 0
    fi
    nc_run /bin/sleep 1 >/dev/null
done

nc_run /bin/rm -f "$TRIGGER" >/dev/null
echo "grab: no frame after ~15 s - is the running binary built with the grab trigger?" >&2
exit 1
