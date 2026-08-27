#!/bin/sh
# tr2-extract-music.sh — regenerate Temple Run 2's music PCM for the FMOD pump
# fallback (APKENV_FMOD_MUSIC_PCM), straight from the game apk.
#
#   usage: apkenv/tools/tr2-extract-music.sh [templerun2.apk] [out-dir]
#   default: packaging/templerun2.apk -> packaging/extras/templerun2/
#
# Why this exists: Unity's own native FMOD stream is primed (64 KB) and then
# never consumed, so the game plays effects but no music (see
# plan/TEMPLERUN2-RENDER-INPUT.md). The shipped workaround mixes the game's own
# music track into the known-good FMOD AudioTrack pump (audio/fmod_pump.c), so
# the pump needs the track as raw PCM at FMOD's measured output format.
#
# The asset: assets/bin/Data/sharedassets0.assets.resS (1277793 bytes, STORED in
# the apk) is two CONCATENATED 44.1 kHz stereo MP3s with no container around
# them — Unity keeps only offsets/lengths, in sharedassets0.assets. The split is
# at 0xe4800: track 1 = 935936 bytes / 60.08 s (the in-game music, the one we
# ship), track 2 = 341857 bytes / 23.47 s. Decoding the whole .resS in one pass
# would concatenate both tracks; the head -c split is the point of this script.
#
# Output format is dictated by the pump, not by taste: fmod_pump.c mixes the
# file into FMOD's own chunk buffer with no resampling, so it MUST be raw
# little-endian stereo S16 at 24000 Hz — FMOD's measured rate on the TouchPad
# ([FMOD] init rate=24000). Result: 5765328 bytes, md5 988d1789aedd4b84c01b83670eafa959.
#
# The output is copyrighted game audio: it is gitignored and must never be
# committed. Regenerate it from your own apk instead.
set -e
cd "$(dirname "$0")/.."

APK="${1:-packaging/templerun2.apk}"
OUTDIR="${2:-packaging/extras/templerun2}"
OUT="$OUTDIR/templerun2-game-24000-s16le.pcm"

RESS=assets/bin/Data/sharedassets0.assets.resS
TRACK1_BYTES=935936          # 0xe4800 — end of the first MP3
RATE=24000                   # FMOD's measured output rate on the TouchPad
EXPECT_MD5=988d1789aedd4b84c01b83670eafa959

[ -f "$APK" ] || { echo "no such apk: $APK" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

unzip -o -q "$APK" "$RESS" -d "$TMP"
[ -f "$TMP/$RESS" ] || { echo "$RESS not in $APK" >&2; exit 1; }

head -c "$TRACK1_BYTES" "$TMP/$RESS" > "$TMP/track1.mp3"
ffmpeg -v error -y -f mp3 -i "$TMP/track1.mp3" \
       -f s16le -acodec pcm_s16le -ar "$RATE" -ac 2 "$TMP/track1.pcm"

mkdir -p "$OUTDIR"
cp "$TMP/track1.pcm" "$OUT"

GOT=$(md5sum "$OUT" | cut -d' ' -f1)
echo "$OUT  $(stat -c '%s' "$OUT") bytes  md5 $GOT"
if [ "$GOT" != "$EXPECT_MD5" ]; then
    echo "WARNING: md5 differs from the shipped 1.3.8 build ($EXPECT_MD5)." >&2
    echo "         Different apk revision or ffmpeg decoder — check it sounds right." >&2
fi

cat <<USAGE

Package it with:
  APPID=com.apkenv.templerun2 APK=packaging/templerun2.apk \\
  APPINFO=packaging/templerun2/appinfo.json ENVFILE=packaging/templerun2/apkenv.env \\
  HOSTLIBS=hostlibs/webos EXTRAS=$OUTDIR ./packaging/build-ipk.sh
USAGE
