#!/bin/bash
# build-ipk.sh — assemble the webOS PDK .ipk for the apkenv WMW runtime.
#
# Ships ONLY the runtime (apkenv binary + harvested gingerbread bionic libs +
# device libEGL + icon/appinfo). The copyrighted game apk and its extracted
# asset root are NOT bundled — they stay on the writable /media/internal
# partition (the binary defaults to /media/internal/wheresmywater.apk via
# APKENV_DEFAULT_APK). This sidesteps both the .ipk size and the game
# redistribution question, and matches the current on-device layout.
#
# Builds a SELF-CONTAINED .ipk: the apkenv runtime + harvested bionic libs +
# device libEGL + the game .apk, all inside the app dir. A fresh device needs
# nothing pre-staged. App-dir layout (see packaging/webos/README, shipped too):
#   apkenv  appinfo.json  icon.png  README
#   libs/webos/   <- webOS-side host libs apkenv loads
#   android/      <- the Android game .apk (engine .so + assets read from here)
#
# Inputs (defaults in parens):
#   $1 / libs/webos      the FOSS bionic runtime libs (committed in the repo).
#   $APK / packaging/wheresmywater.apk   the game apk to bundle (the patched,
#                        known-good one — NOT committed; bring your own).
# libEGL.so (HP-proprietary) is harvested from a device into devlibs/ by
# build-webos.sh; this script just copies it from there.
#
# Usage: packaging/build-ipk.sh [path-to-bionic-libs-dir]
set -e
cd "$(dirname "$0")/.."          # -> apkenv/

APPID=com.apkenv.wheresmywater
STAGE=packaging/stage/$APPID
LIBS=${1:-libs/webos}                    # FOSS bionic .so's (committed)
APK=${APK:-packaging/wheresmywater.apk}  # game apk to bundle

[ -f apkenv ] || { echo "build the binary first: ./build-webos.sh"; exit 1; }
[ -f devlibs/libEGL.so ] || { echo "missing devlibs/libEGL.so — run ./build-webos.sh (harvests it from a device)"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/libs/webos" "$STAGE/android"

cp apkenv                       "$STAGE/apkenv"
cp packaging/webos/appinfo.json "$STAGE/appinfo.json"
cp packaging/webos/README       "$STAGE/README"
chmod +x "$STAGE/apkenv"
cp devlibs/libEGL.so            "$STAGE/libs/webos/"   # harvested device lib

# bundle the Android game (the "android bits")
if [ -f "$APK" ]; then
    cp "$APK" "$STAGE/android/$(basename "$APK")"
    echo "bundled game apk: android/$(basename "$APK") ($(stat -c%s "$APK") bytes)"
else
    echo "WARNING: game apk '$APK' not found — the .ipk will not be self-contained."
    echo "         The binary will fall back to APKENV_DEFAULT_APK on /media/internal."
fi

# Launcher icon: extract from the game apk at package time (not committed, as
# it is copyrighted game art). Falls back to a committed icon if present.
APK=${APK:-../android-candidates/wheresmywater_1.0.2.apk}
if [ -f packaging/webos/icon.png ]; then
    cp packaging/webos/icon.png "$STAGE/icon.png"
elif [ -f "$APK" ]; then
    unzip -o -q "$APK" res/drawable-hdpi/iconfree.png -d packaging/.icontmp
    if command -v convert >/dev/null 2>&1; then
        convert packaging/.icontmp/res/drawable-hdpi/iconfree.png -resize 64x64\! "$STAGE/icon.png"
    else
        cp packaging/.icontmp/res/drawable-hdpi/iconfree.png "$STAGE/icon.png"
    fi
    rm -rf packaging/.icontmp
else
    echo "WARNING: no icon.png and no apk to extract one from"
fi

if [ -d "$LIBS" ] && ls "$LIBS"/*.so >/dev/null 2>&1; then
    cp "$LIBS"/*.so "$STAGE/libs/webos/" 2>/dev/null || true
    echo "bundled bionic libs from $LIBS:"
    ls "$STAGE/libs/webos/"
else
    echo "WARNING: bionic libs dir '$LIBS' has no .so — the .ipk will install but"
    echo "         WILL NOT RUN without libc/libm/libstdc++/liblog/libz in libs/webos/."
    echo "         These are committed at apkenv/libs/webos/; restore them if missing."
fi

# palm-package lives in PalmSDK (/usr/local/bin or /opt/PalmSDK/Current/bin).
OUT=packaging/out
mkdir -p "$OUT"
palm-package --outdir "$OUT" "$STAGE"
echo "DONE: $(ls -1 "$OUT"/*.ipk 2>/dev/null | tail -1)"
echo "Install on device:  palm-install <that>.ipk    (or on-device: ipkg install)"
