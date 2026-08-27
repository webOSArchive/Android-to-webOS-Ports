#!/bin/bash
# build-mono-webos.sh - build Unity's Mono 2.6.5 runtime for webOS (glibc/ARM).
#
# WHY: Temple Run 2 is Unity 3.5.7f6 + Mono. The apk ships a *bionic* libmono.so
# that dies during mono_jit_init_version() under apkenv's bionic->glibc bridge:
# a JIT + GC + signal-handler + thread-attach stack is the single most
# libc-hostile thing in any of these games, and each mismatch is a silent memory
# corruption rather than a missing contract. Building the SAME Mono against the
# device's own glibc removes that whole class of bug at once. apkenv then bridges
# libunity's mono_* imports to this library (compat/hostlib.c).
#
# The pin is exact and verified: this source tree produces a libmono.so whose
# dynamic export set is *identical* (910 symbols, zero difference in either
# direction) to the bionic libmono.so shipped inside templerun2_1.2.1.apk.
#
# Output: hostlibs/webos/libmono-webos.so       (stripped, ships to the device)
#         build/webos/libmono-webos.so.debug    (unstripped, for addr2line)
#
# Prereqs: PalmPDK at /opt/PalmPDK, host autoconf/automake/libtool/perl, git.
set -e
cd "$(dirname "$0")/.."
APKENV="$(pwd)"

MONO_REPO=https://github.com/Unity-Technologies/mono.git
MONO_BRANCH=unity3.5
# Verified pin: Mono 2.6.5, MONO_CORLIB_VERSION 82, matches Unity 3.5.7f6.
MONO_COMMIT=64c3378a67376d089f8ad6f7b6cad4619fdaefa9

SRCDIR="${MONO_SRC_DIR:-${TMPDIR:-/tmp}/mono-unity3.5}"
PDK=/opt/PalmPDK/arm-gcc
P=$PDK/bin/arm-none-linux-gnueabi

[ -d "$PDK" ] || { echo "ERROR: PalmPDK not found at $PDK" >&2; exit 1; }

# ---- 1. source -------------------------------------------------------------
if [ ! -d "$SRCDIR/.git" ]; then
    echo "CLONE $MONO_REPO ($MONO_BRANCH) -> $SRCDIR"
    git clone --branch "$MONO_BRANCH" "$MONO_REPO" "$SRCDIR"
fi
cd "$SRCDIR"
git checkout -q "$MONO_COMMIT" 2>/dev/null || {
    echo "ERROR: commit $MONO_COMMIT not found; is the clone shallow?" >&2; exit 1; }

# Two source fixes, both required, both mechanical (see tools/mono-webos.patch):
#  1. mono/utils/mono-filemap.c - mono_file_map_override() is wrapped in
#     "#if defined(ANDROID)". libunity calls it unconditionally to map assemblies
#     out of the apk, so it must exist. We do NOT just pass -DANDROID: that also
#     flips libgc/include/private/gcconfig.h and libgc/pthread_stop_world.c onto
#     Android-kernel code paths that are wrong for webOS.
#  2. runtime/Makefile.am - "AUTOMAKE_OPTIONS = cygnus" was removed in automake
#     1.13; it is only a hack to keep 'check' from depending on 'all'.
if ! git diff --quiet; then
    echo "PATCH already applied (working tree dirty) - skipping"
else
    echo "PATCH tools/mono-webos.patch"
    git apply "$APKENV/tools/mono-webos.patch"
fi

# ---- 2. autotools ----------------------------------------------------------
# The tree is from 2009; autoconf 2.71 / automake 1.16 emit many deprecation
# warnings but succeed once the cygnus option above is gone.
echo "AUTORECONF eglib"; (cd eglib && autoreconf -i >/dev/null 2>&1)
echo "AUTORECONF ."; autoreconf -i >/dev/null 2>&1

# ---- 3. configure ----------------------------------------------------------
export PATH=$PDK/bin:$PATH
export CC="${P}-gcc" CXX="${P}-g++" CPP="${P}-cpp" CXXCPP="${P}-cpp"
export LD="${P}-ld" AR="${P}-ar" AS="${P}-as" RANLIB="${P}-ranlib"
export STRIP="${P}-strip" NM="${P}-nm"

# ARM flags follow Unity's own build_runtime_android.sh armv7a line: VFP (not
# NEON) because that is the FPU Mono's ARM JIT emits for, and softfp to match
# both libunity and everything else apkenv links.
#
# -fgnu89-inline is REQUIRED and was the one real build blocker. eglib hardcodes
# -D_FORTIFY_SOURCE=2 (eglib/src/Makefile.am), which pulls in glibc 2.5's
# bits/stdio2.h & friends. Those use a bare "extern __always_inline"; under
# gcc 4.3's default -std=gnu99 that emits an external definition in EVERY
# translation unit, so linking libeglib.a fails with "multiple definition of
# realpath/fgets/gets/stpncpy/...". -fgnu89-inline restores gnu89 extern-inline
# semantics (definition emitted nowhere) while leaving the rest of C99 alone.
export CFLAGS="-march=armv7-a -mfloat-abi=softfp -mfpu=vfp -DARM_FPU_VFP=1 -DHAVE_ARMV6=1 -fpic -g -O2 -fgnu89-inline"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-Wl,--fix-cortex-a8"

# mono_cv_uscore: whether dlsym() needs a leading underscore. It is an
# AC_TRY_RUN, so cross builds cannot answer it and it MUST be set here.
# ELF/glibc does not use a prefix -> "no". (Unity's Android script says "yes";
# that is a bionic quirk. Getting this wrong breaks "__Internal" P/Invokes.)
rm -f "$SRCDIR/webos_cross.cache"
./configure \
    --host=arm-none-linux-gnueabi --build=x86_64-pc-linux-gnu \
    --prefix="$SRCDIR/install" --cache-file="$SRCDIR/webos_cross.cache" \
    --disable-mcs-build --disable-parallel-mark --with-sigaltstack=no \
    --with-tls=pthread --with-glib=embedded --disable-nls --with-gc=included \
    mono_cv_uscore=no

# ---- 4. build --------------------------------------------------------------
make -j"$(nproc)"

LIB=mono/mini/.libs/libmono.so.0.0.0
[ -f "$LIB" ] || { echo "ERROR: $LIB not produced" >&2; exit 1; }

# ---- 5. install + verify ---------------------------------------------------
mkdir -p "$APKENV/hostlibs/webos" "$APKENV/build/webos"
cp "$LIB" "$APKENV/build/webos/libmono-webos.so.debug"
"${P}-strip" -o "$APKENV/hostlibs/webos/libmono-webos.so" "$LIB"

echo
echo "=== verify ==="
file "$APKENV/hostlibs/webos/libmono-webos.so"
echo -n "max glibc symbol version: "
readelf -V --dyn-syms "$APKENV/hostlibs/webos/libmono-webos.so" \
    | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1
readelf -d "$APKENV/hostlibs/webos/libmono-webos.so" | grep NEEDED
"$APKENV/tools/check-mono-exports.sh" "$APKENV/hostlibs/webos/libmono-webos.so"
