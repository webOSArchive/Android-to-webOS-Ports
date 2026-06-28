#!/bin/bash
# build-webos.sh — two-toolchain cross-build of apkenv for HP TouchPad / webOS.
#
# WHY two toolchains (hard-won, do not "simplify"):
#   - COMPILE with host arm-linux-gnueabi-gcc-13: it tolerates the duplicate
#     GLES1+GLES2 typedefs that every GL header pulls in (PalmPDK gcc 4.3.3
#     rejects them). But force it onto the OLD PalmPDK glibc headers via
#     -nostdinc + -isystem, else it emits __isoc23_* / GLIBC_2.38 symbol refs
#     the device lacks.
#   - LINK with PalmPDK arm-none-linux-gnueabi-gcc (4.3.3) so symbols bind to
#     glibc 2.4. Device GLES1 lib is the OLD name libGLES_CM.so.
#
# Output: ./apkenv  (ARM, interp /lib/ld-linux.so.3, highest sym GLIBC_2.4)
set -e
cd "$(dirname "$0")"

CC13=arm-linux-gnueabi-gcc-13
GCC13_INC=$(${CC13} -print-file-name=include)
PDK=/opt/PalmPDK
PDK_SYSROOT_INC=$PDK/arm-gcc/sysroot/usr/include
LD=$PDK/arm-gcc/bin/arm-none-linux-gnueabi-gcc

BUILD=build/webos
mkdir -p "$BUILD"

# ---- generated GLES serializer (python) ----
if [ ! -f compat/gen/gles_serialize.c ]; then
    echo "GEN compat/gen/gles_serialize.c"
    mkdir -p compat/gen
    python3 compat/wrapgen2.py compat/gles_vtable.h compat/gen/gles_serialize.c
fi

CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=softfp -fsigned-char -O2 -fPIC"
CFLAGS="$CFLAGS -nostdinc -isystem $GCC13_INC -isystem $PDK_SYSROOT_INC"
CFLAGS="$CFLAGS -Iglshim -I$PDK/include -I$PDK/include/SDL -I."
CFLAGS="$CFLAGS -D_GNU_SOURCE -D_BSD_SOURCE -D_TIME_BITS=32 -D__webos__ -DLINUX"
# NB: modern Khronos glshim headers already typedef GLchar — do NOT -DGLchar=char.
CFLAGS="$CFLAGS -DAPKENV_GLES -DAPKENV_GLES2"
CFLAGS="$CFLAGS -DAPKENV_STATIC_MODULES -DAPKENV_LATEHOOKS -DLINKER_DEBUG=0"
# harvested bionic libs live here on-device (play.sh cd's to /var/apkenv first)
CFLAGS="$CFLAGS -DAPKENV_LOCAL_BIONIC_PATH=\"./libs/webos/\""
CFLAGS="$CFLAGS -DAPKENV_PREFIX=\"/usr\" -DAPKENV_TARGET=\"webos\" -DAPKENV_PLATFORM=\"webos\""
# Default apk when launched from the webOS launcher (no argv). Game data stays
# on the writable /media/internal partition (the runtime .ipk ships only the
# apkenv binary + harvested bionic libs, not the copyrighted game).
CFLAGS="$CFLAGS -DAPKENV_DEFAULT_APK=\"/media/internal/wheresmywater.apk\""
CFLAGS="$CFLAGS -Wno-deprecated-declarations -fgnu89-inline -fno-builtin -fno-stack-protector"
CFLAGS="$CFLAGS -include compat/pdk_compat.h"

# ---- source list (mirrors makefile SOURCES, but only platform/webos.c and
#      only the wheresmywater module) ----
SOURCES="apkenv.c"
SOURCES="$SOURCES $(ls linker/*.c)"
SOURCES="$SOURCES $(ls compat/*.c) compat/gen/gles_serialize.c"
SOURCES="$SOURCES $(ls apklib/*.c)"
SOURCES="$SOURCES $(ls jni/*.c)"
SOURCES="$SOURCES $(ls imagelib/*.c)"
SOURCES="$SOURCES $(ls debug/*.c)"
SOURCES="$SOURCES $(ls accelerometer/*.c)"
SOURCES="$SOURCES $(ls audio/*.c)"
SOURCES="$SOURCES $(ls mixer/*.c)"
SOURCES="$SOURCES $(ls platform/common/*.c)"
SOURCES="$SOURCES platform/webos.c"
SOURCES="$SOURCES modules/wheresmywater.c"

OBJS=""
for src in $SOURCES; do
    obj="$BUILD/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        echo "CC  $src"
        $CC13 $CFLAGS -c "$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done

# ---- debug/wrappers: special flags (thumb/arm, -O0) ----
for src in debug/wrappers/*_thumb.c; do
    [ -e "$src" ] || continue
    obj="$BUILD/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        echo "CC(TH) $src"
        $CC13 $CFLAGS -mthumb -O0 -c "$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done
for src in debug/wrappers/*_arm.c; do
    [ -e "$src" ] || continue
    obj="$BUILD/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        echo "CC(ARM) $src"
        $CC13 $CFLAGS -marm -O0 -c "$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done

# ---- link with PalmPDK 4.3.3 ----
echo "LINK apkenv"
$LD $OBJS -o apkenv \
    -L$PDK/device/lib -Ldevlibs \
    -Wl,--allow-shlib-undefined \
    -rdynamic -pthread -ldl -lz -lrt -lm \
    -lSDL -lSDL_mixer -lpdl -lGLES_CM -lGLESv2 -lEGL -lstdc++

echo "DONE: $(file apkenv | cut -d, -f1-2)"
echo "highest glibc sym: $(arm-linux-gnueabi-readelf -V apkenv 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
