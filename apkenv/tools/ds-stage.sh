#!/bin/bash
# ds-stage.sh - build the two packaging payloads for Dead Space (EA BLAST).
#
# WHY this exists rather than "just bundle the apk" (the pattern every other
# port here uses): the BLAST engine opens its content with plain fopen/mmap on
# relative paths ("published/sounds/soundBase.sb"). It has no zip reader for
# assets and no AAssetManager, so it CANNOT read them out of the apk the way
# Fruit Ninja's Mortar engine does. The content has to exist as real files.
#
# Bundling the whole 296 MB apk *and* the 319 MB extracted tree would make a
# ~615 MB package, over half of it dead weight. So:
#
#   APK   -> a stripped apk: lib/ + AndroidManifest.xml + resources.arsc + res/
#            (6.6 MB). apkenv only needs it to find and load the engine .so,
#            to name the app, and for build-ipk.sh to pull the launcher icon.
#   DATA  -> assets/published/ as a real directory tree (319 MB), shipped in
#            the app dir and read in place. apkenv chdir()s to the app dir at
#            startup, so the engine's relative paths resolve with NO copy and
#            no first-run seeding.
#
# Total ~326 MB instead of ~615 MB, and nothing is duplicated on the device.
#
# Usage:  apkenv/tools/ds-stage.sh [<source apk>] [<outdir>]
# Output: <outdir>/deadspace-stripped.apk
#         <outdir>/content/published/...
# Then:   APPID=com.apkenv.deadspace APK=<outdir>/deadspace-stripped.apk \
#         DATA=<outdir>/content APPINFO=packaging/deadspace/appinfo.json \
#         ENVFILE=packaging/deadspace/apkenv.env packaging/build-ipk.sh
set -e
cd "$(dirname "$0")/.."

SRC=${1:-../android-candidates/Dead-Space.apk}
OUT=${2:-packaging/extras/deadspace}
[ -f "$SRC" ] || { echo "no such apk: $SRC" >&2; exit 1; }
command -v zip >/dev/null || { echo "need 'zip'" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== unpacking $SRC"
unzip -oq "$SRC" -d "$WORK"

mkdir -p "$OUT"

echo "== stripped apk (engine + manifest + resources, no assets)"
rm -f "$OUT/deadspace-stripped.apk"
( cd "$WORK" && zip -q -r -X "$OLDPWD/$OUT/deadspace-stripped.apk" \
      lib AndroidManifest.xml resources.arsc res )

# The engine composes an ABSOLUTE content path as
#   <GetExternalStorageDirectory()>/Android/data/com.ea.deadspace/files/published/...
# (observed on device, ds-07: stat of exactly that, failing). That middle part
# is fixed in the binary, so the staged tree has to reproduce it - the module
# answers GetExternalStorageDirectory() with <appdir>/android/extras and the
# engine appends the rest. Note the package id in the path is com.ea.deadspace,
# NOT the apk's com.eamobile.deadspace_full_azn.
CONTENT_SUBDIR=Android/data/com.ea.deadspace/files
echo "== content tree at $CONTENT_SUBDIR/published (read in place on the device)"
rm -rf "$OUT/content"
mkdir -p "$OUT/content/$CONTENT_SUBDIR"
cp -r "$WORK/assets/published" "$OUT/content/$CONTENT_SUBDIR/published"

# The engine also reads a couple of small config files through Java's
# AssetManager (EAMCore.ini and friends), not just the published/ tree it
# fopen()s. Stripping assets/ out of the apk took those with it, so stage them
# alongside; modules/eablast.c serves AssetManager.open() from here.
mkdir -p "$OUT/content/assets"
find "$WORK/assets" -maxdepth 1 -type f -exec cp {} "$OUT/content/assets/" \;
echo "== staged $(ls "$OUT/content/assets" | wc -l) loose asset file(s): $(ls "$OUT/content/assets" | tr '\n' ' ')"

# Verify, loudly. A short content tree is a game that boots and then fails to
# find a level, which is a much more expensive thing to debug on the device.
SRC_N=$(find "$WORK/assets/published" -type f | wc -l)
OUT_N=$(find "$OUT/content/$CONTENT_SUBDIR/published" -type f | wc -l)
SRC_B=$(du -sb "$WORK/assets/published" | cut -f1)
OUT_B=$(du -sb "$OUT/content/$CONTENT_SUBDIR/published" | cut -f1)

echo
echo "stripped apk : $(du -h "$OUT/deadspace-stripped.apk" | cut -f1)"
echo "content tree : $OUT_N files, $(du -sh "$OUT/content" | cut -f1)"
if [ "$SRC_N" != "$OUT_N" ] || [ "$SRC_B" != "$OUT_B" ]; then
    echo "MISMATCH: apk has $SRC_N files / $SRC_B bytes, staged $OUT_N / $OUT_B" >&2
    exit 1
fi
echo "verified: file count and byte total match the apk exactly"

# The engine .so must have survived the strip - that is the whole point of it.
unzip -l "$OUT/deadspace-stripped.apk" | grep -q "lib/armeabi/libDeadSpace.so" ||
    { echo "stripped apk has no engine .so" >&2; exit 1; }
echo "verified: lib/armeabi/libDeadSpace.so present in the stripped apk"
