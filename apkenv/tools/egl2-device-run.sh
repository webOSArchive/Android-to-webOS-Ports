#!/bin/bash
# egl2-device-run.sh [raw|pdl|sdl|all] - build tools/egl2test.c and run it on the
# TouchPad.  Answers "is an ES2 context obtainable, and under what process state"
# (plan/TEMPLERUN2-RENDER-INPUT.md, Stage G2).
#
# novacom notes (learned the hard way, see plan/TEMPLERUN2-MONO.md):
#   - the "--" separator belongs in nc_run ONLY; repeating it at the call site
#     makes the target see a literal "--" as argv[1].
#   - novacom run's cwd is "/", which webOS mounts read-only. Use /var or
#     /media/internal.
#   - a hung `novacom run` wedges the HOST daemon: sudo systemctl restart novacomd
set -e
cd "$(dirname "$0")/.."
MODE="${1:-all}"
DEV=/var/apkenv2
BUILD=build/webos

CC13=arm-linux-gnueabi-gcc-13
GCC13_INC=$(${CC13} -print-file-name=include)
PDK=/opt/PalmPDK
PDK_SYSROOT_INC=$PDK/arm-gcc/sysroot/usr/include
LD=$PDK/arm-gcc/bin/arm-none-linux-gnueabi-gcc

CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=softfp -fsigned-char -O2"
CFLAGS="$CFLAGS -nostdinc -isystem $GCC13_INC -isystem $PDK_SYSROOT_INC"
CFLAGS="$CFLAGS -Iglshim -I$PDK/include -I$PDK/include/SDL -I."
CFLAGS="$CFLAGS -D_GNU_SOURCE -D_BSD_SOURCE -D_TIME_BITS=32 -D__webos__ -DLINUX"
CFLAGS="$CFLAGS -fgnu89-inline -fno-builtin -fno-stack-protector -Wall"
CFLAGS="$CFLAGS -include compat/pdk_compat.h"

mkdir -p "$BUILD"
echo "CC   tools/egl2test.c"
$CC13 $CFLAGS -c tools/egl2test.c -o "$BUILD/egl2test.o"
echo "LINK $BUILD/egl2test"
$LD "$BUILD/egl2test.o" -o "$BUILD/egl2test" \
    -L$PDK/device/lib -Ldevlibs -Wl,--allow-shlib-undefined \
    -lSDL -lpdl -lEGL -lGLESv2 -lGLES_CM -lm -ldl -lpthread

nc_run() { timeout 300 novacom run "file://$1" -- "${@:2}"; }
nc_put() { timeout 300 novacom put "file://$2.new" < "$1" && \
           timeout 60 novacom run file:///bin/mv -- "$2.new" "$2"; }

[ -n "$(timeout 20 novacom -l 2>/dev/null)" ] || {
    echo "ERROR: no device (or novacomd is wedged: sudo systemctl restart novacomd)" >&2
    exit 1; }

nc_run /bin/mkdir -p $DEV
echo "PUSH egl2test"
nc_put "$BUILD/egl2test" $DEV/egl2test
nc_run /bin/chmod 755 $DEV/egl2test

run_one() {
    echo "=== RUN egl2test $1 ==="
    nc_run $DEV/egl2test "$1" || echo "(exit $?)"
}
if [ "$MODE" = all ]; then
    for m in raw pdl sdl; do run_one $m; done
else
    run_one "$MODE"
fi
