#!/bin/sh
# tr2-extract-splash.sh — turn Temple Run 2's splash art into the raw RGB the
# boot splash in compat/fbo_es2.c uploads (APKENV_SPLASH_RGB).
#
#   usage: apkenv/tools/tr2-extract-splash.sh [templerun2.apk] [out-dir] [WxH] [cover|contain]
#   default: packaging/templerun2.apk -> packaging/extras/templerun2/ 768x1024 cover
#
# Why raw and not the PNG: apkenv links no image decoder, and decoding at
# package time is the same trade already made for the music bed — no runtime
# dependency, and the file arrives pre-scaled for this exact panel.
#
# Two conventions the file MUST follow, both invisible until it renders wrong:
#
#  - **Bottom row first.** GL's texture origin is bottom-left, so texel row 0 is
#    the BOTTOM of the image. ffmpeg writes top-down, hence `vflip`. Get this
#    wrong and the splash appears upside down (unmistakable — the studio banner
#    lands at the top, mirrored).
#  - **Portrait, at the FBO's size, not the panel's.** The splash rides the same
#    rotated quad as the game's frames (compat/fbo_es2.c), so it is a 768x1024
#    portrait image and the present rotates it onto the 1024x768 panel. Do not
#    pre-rotate it.
#
# Scaling: the source is 640x1136 (an iPhone-5-shaped launch image) and the
# panel's portrait target is 768x1024, so the aspects genuinely differ. `cover`
# scales to fill and centre-crops ~170 px off top and bottom — right for this
# art, which is full-bleed and keeps the IMANGI banner well inside the crop.
# `contain` letterboxes instead, if a game's splash has content near the edges.
set -e
cd "$(dirname "$0")/.."

APK="${1:-packaging/templerun2.apk}"
OUTDIR="${2:-packaging/extras/templerun2}"
SIZE="${3:-768x1024}"
MODE="${4:-cover}"

ASSET=assets/bin/Data/splash.png
W=${SIZE%x*}
H=${SIZE#*x}
OUT="$OUTDIR/templerun2-splash-${W}x${H}-rgb.raw"

[ -f "$APK" ] || { echo "no such apk: $APK" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }

case "$MODE" in
    cover)   VF="scale=$W:$H:force_original_aspect_ratio=increase,crop=$W:$H,vflip" ;;
    contain) VF="scale=$W:$H:force_original_aspect_ratio=decrease,pad=$W:$H:(ow-iw)/2:(oh-ih)/2:black,vflip" ;;
    *)       echo "mode must be cover or contain" >&2; exit 1 ;;
esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

unzip -o -q "$APK" "$ASSET" -d "$TMP"
[ -f "$TMP/$ASSET" ] || { echo "$ASSET not in $APK" >&2; exit 1; }

mkdir -p "$OUTDIR"
ffmpeg -v error -y -i "$TMP/$ASSET" -vf "$VF" -f rawvideo -pix_fmt rgb24 "$OUT"

EXPECT=$((W * H * 3))
GOT=$(stat -c '%s' "$OUT")
echo "$OUT  ${GOT} bytes (${W}x${H} RGB, $MODE)"
[ "$GOT" = "$EXPECT" ] || { echo "expected $EXPECT bytes" >&2; exit 1; }

cat <<USAGE

Launch settings (packaging/templerun2/apkenv.env):
  APKENV_SPLASH_RGB=android/extras/$(basename "$OUT")
  APKENV_SPLASH_SIZE=${W}x${H}
USAGE
