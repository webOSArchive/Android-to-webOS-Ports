#!/bin/bash
# rc-stage.sh - build the working apk for RoboCop 3.0.6 (Glu, Unity 4.2.2f1).
#
# WHY: like Temple Run 2 and Aralon, the apk's lib/armeabi-v7a/libunity.so
# (47 KB) and libmono.so (93 KB) are PROXIES. Unlike them, the real engine is not
# in the apk's assets/libs/ but inside the expansion OBB. Loading the proxy made
# its JNI_OnLoad look for 'assets/libs/armeabi-v7a/libunity.so', find nothing,
# register no natives, and the first nativeFile() call jumped to NULL
# (plan/logs/robocop-r1.log).
#
# apkenv loads every .so from the FIRST of assets/libs/armeabi-v7a, ...,
# lib/armeabi-v7a that has any, so this adds to a copy of the pristine apk:
#   assets/libs/armeabi-v7a/libunity.so  (from the OBB, the real engine)
#   assets/libs/armeabi-v7a/libmono.so   (from the OBB; blacklisted at runtime
#                                         in favour of the host Mono, kept so the
#                                         apk mirrors what the game ships)
#   assets/libs/armeabi-v7a/libmain.so   (from the apk's lib/: libunity NEEDs it)
#   assets/libs/armeabi-v7a/libgwallet.so (from the apk's lib/: C# [DllImport("gwallet")],
#                                         resolved by apkenv's Mono P/Invoke fallback)
# No bytes of any library are changed, and the OBB ships unmodified.
#
# Usage: apkenv/tools/rc-stage.sh   -> apkenv/packaging/robocop.apk
set -e
cd "$(dirname "$0")/.."
SRC_APK=../android-candidates/robocop_3.0.6.apk
SRC_OBB=../android-candidates/main.1309.com.glu.robocop.obb
OUT=packaging/robocop.apk
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

cp "$SRC_APK" "$TMP/robocop.apk"
(cd "$TMP" && unzip -q "$OLDPWD/$SRC_OBB" 'assets/libs/armeabi-v7a/libunity.so' \
                                          'assets/libs/armeabi-v7a/libmono.so' \
    && unzip -q -j "$OLDPWD/$SRC_APK" lib/armeabi-v7a/libmain.so lib/armeabi-v7a/libgwallet.so \
                                      -d assets/libs/armeabi-v7a \
    && zip -q -0 robocop.apk assets/libs/armeabi-v7a/libunity.so \
                             assets/libs/armeabi-v7a/libmono.so \
                             assets/libs/armeabi-v7a/libmain.so \
                             assets/libs/armeabi-v7a/libgwallet.so)
cp "$TMP/robocop.apk" "$OUT"
mkdir -p packaging/extras/robocop
[ "$(md5sum < packaging/extras/robocop/main.1309.com.glu.robocop.obb 2>/dev/null | cut -d' ' -f1)" = \
  00da7034bad96d2ac90fe097583cd77f ] || cp "$SRC_OBB" packaging/extras/robocop/
unzip -l "$OUT" | grep '\.so$'
md5sum "$OUT" packaging/extras/robocop/*.obb
