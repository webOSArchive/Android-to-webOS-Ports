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
# Optional per-game extras (env):
#   DATA=<dir>      extracted resource tree, shipped as android/<apk>.data/ and
#                   seeded into /media/internal/.apkenv/<apk>/ on first launch
#   ENVFILE=<file>  KEY=VALUE launch settings, shipped as android/apkenv.env
#   EXTRAS=<dir>    additional runtime files, shipped under android/extras/
#   APPINFO, ICON, README   overrides (see below)
#
# Usage: packaging/build-ipk.sh [path-to-bionic-libs-dir]
set -e
cd "$(dirname "$0")/.."          # -> apkenv/

APPID=${APPID:-com.apkenv.wheresmywater}
STAGE=packaging/stage/$APPID
LIBS=${1:-libs/webos}                    # FOSS bionic .so's (committed)
APK=${APK:-packaging/wheresmywater.apk}  # game apk to bundle

[ -f apkenv ] || { echo "build the binary first: ./build-webos.sh"; exit 1; }
[ -f devlibs/libEGL.so ] || { echo "missing devlibs/libEGL.so — run ./build-webos.sh (harvests it from a device)"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/libs/webos" "$STAGE/android"

cp apkenv                       "$STAGE/apkenv"
cp "${APPINFO:-packaging/webos/appinfo.json}" "$STAGE/appinfo.json"
cp "${README:-packaging/webos/README}" "$STAGE/README"
chmod +x "$STAGE/apkenv"
cp devlibs/libEGL.so            "$STAGE/libs/webos/"   # harvested device lib

# bundle the Android game (the "android bits")
if [ -f "$APK" ]; then
    cp "$APK" "$STAGE/android/$(basename "$APK")"
    echo "bundled game apk: android/$(basename "$APK") ($(stat -c%s "$APK") bytes)"
    if [ -n "${DATA:-}" ] && [ -d "$DATA" ]; then
        cp -r "$DATA" "$STAGE/android/$(basename "$APK").data"
        echo "bundled data tree: android/$(basename "$APK").data ($(du -sh "$DATA" | cut -f1))"
    fi
    if [ -n "${ENVFILE:-}" ] && [ -f "$ENVFILE" ]; then
        cp "$ENVFILE" "$STAGE/android/apkenv.env"
        echo "bundled launch env: $(tr '\n' ' ' < "$ENVFILE")"
    fi
else
    echo "WARNING: game apk '$APK' not found — the .ipk will not be self-contained."
    echo "         The binary will fall back to APKENV_DEFAULT_APK on /media/internal."
fi

if [ -n "${EXTRAS:-}" ] && [ -d "$EXTRAS" ]; then
    mkdir -p "$STAGE/android/extras"
    cp -r "$EXTRAS"/. "$STAGE/android/extras/"
    echo "bundled extras from $EXTRAS:"
    ls -la "$STAGE/android/extras/" | tail -n +2
fi

# Launcher icon: extract the largest available app icon from the game apk at
# package time (not committed — copyrighted game art). Never fatal.
#
# Deterministic on purpose. This used to unzip every candidate into a temp dir
# and take `find | head -1`, which is filesystem-order dependent — so a rebuild
# could silently swap the icon, and Fruit Ninja shipped the paid artwork while
# its manifest declares the free one. Now: ask aapt what the manifest actually
# points at, prefer that name's "_large" variant, walk densities high to low,
# take the FIRST hit, and say which file was used.
if [ -n "${ICON:-}" ] && [ -f "$ICON" ]; then
    cp "$ICON" "$STAGE/icon.png"
    echo "icon: $ICON (explicit)"
elif [ -f "$APK" ]; then
    rm -rf packaging/.icontmp && mkdir -p packaging/.icontmp

    # The manifest's own icon, e.g. res/drawable-mdpi/icon_free.png -> icon_free
    manifest_icon=""
    if command -v aapt >/dev/null 2>&1; then
        manifest_icon=$(aapt dump badging "$APK" 2>/dev/null |
            sed -n "s/^application:.*icon='res\/[^/]*\/\([^']*\)\.png'.*/\1/p" | head -1)
    fi

    # Bigger art first: "<name>_large" beats "<name>".
    names=""
    [ -n "$manifest_icon" ] && names="${manifest_icon}_large $manifest_icon"
    names="$names iconfree_large iconfree icon_large icon ic_launcher app_icon"

    src=""
    for n in $names; do
        for d in drawable-xxhdpi drawable-xhdpi drawable-hdpi drawable drawable-mdpi; do
            if unzip -o -q "$APK" "res/$d/$n.png" -d packaging/.icontmp 2>/dev/null &&
               [ -s "packaging/.icontmp/res/$d/$n.png" ]; then
                src="packaging/.icontmp/res/$d/$n.png"
                break 2
            fi
        done
    done

    if [ -n "$src" ]; then
        if command -v convert >/dev/null 2>&1; then
            convert "$src" -filter Lanczos -resize 64x64\! "$STAGE/icon.png"
        else
            cp "$src" "$STAGE/icon.png"
        fi
        echo "icon: ${src#packaging/.icontmp/} ($(identify -format '%wx%h' "$src" 2>/dev/null))"
    else
        echo "WARNING: no app icon found in apk — launcher will show a placeholder"
    fi
    rm -rf packaging/.icontmp
fi

# Host (glibc) libraries bridged in by compat/hostlib.c - e.g. the natively-built
# Mono runtime that replaces Unity's bionic libmono.so. These are NOT bionic libs
# and must never land in libs/webos/, which is the bionic linker's search path.
# apkenv chdir()s to its own directory, so a run-dir-relative APKENV_HOST_MONO
# in the env file resolves against the installed app dir.
if [ -n "${HOSTLIBS:-}" ] && [ -d "$HOSTLIBS" ] && ls "$HOSTLIBS"/*.so >/dev/null 2>&1; then
    mkdir -p "$STAGE/hostlibs/webos"
    cp "$HOSTLIBS"/*.so "$STAGE/hostlibs/webos/"
    echo "bundled host libs from $HOSTLIBS:"
    ls -la "$STAGE/hostlibs/webos/" | tail -n +2
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
# Name THIS package, not whatever sorts last in the output dir.
echo "DONE: $(ls -1t "$OUT"/${APPID}_*.ipk 2>/dev/null | head -1)"
echo "Install on device:  palm-install <that>.ipk    (or on-device: ipkg install)"
