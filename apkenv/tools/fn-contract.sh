#!/bin/bash
# fn-contract.sh <game.apk> [<engine lib basename>] - regenerate the engine->Java
# host contract table (PORTING-PLAYBOOK.md §2) for a Halfbrick Mortar apk.
#
# Prints, for every Java method name the engine's string table actually
# references: the class that defines it, its JNI signature, and its modifiers.
# A "native" modifier means the entry is one of the ENGINE'S OWN callbacks,
# bound by RegisterNatives from JNI_OnLoad — answer those by calling the
# registered function (jnienv_find_native_method), not by inventing a value.
# Everything else is a host service the module owes the game.
#
#   apkenv/tools/fn-contract.sh android-candidates/fruitninja_1.8.8.apk \
#       > plan/fruitninja-contract.txt
#
# Needs: baksmali, unzip, binutils (strings).
set -e
APK=${1:?usage: fn-contract.sh <game.apk> [<engine lib basename>]}
LIB=${2:-libmortargame.so}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

unzip -oq "$APK" -d "$WORK"
baksmali d "$WORK/classes.dex" -o "$WORK/smali" >/dev/null

SO=$(find "$WORK/lib" -name "$LIB" | head -1)
[ -n "$SO" ] || { echo "no $LIB in $APK" >&2; exit 1; }

PKGDIR=$(dirname "$(find "$WORK/smali" -name 'NativeGameLib.smali' | head -1)")
VENDOR=$(dirname "$PKGDIR")                     # .../smali/com/halfbrick

# Every method name defined in the GAME'S OWN packages. Scoping this to the
# vendor tree matters: the apk also bundles MoPub, AdColony, Vungle, Google
# Play and friends, whose generic method names (init, start, read, close,
# values, ...) collide with unrelated strings in the engine binary and bury
# the real contract in 900 lines of noise.
grep -rhoE '^\.method[^(]* [a-zA-Z_$0-9]+\(' "$VENDOR" | awk '{print $NF}' | tr -d '(' |
    sort -u > "$WORK/javanames"
# ...intersected with the names the engine binary actually references.
strings -n 4 "$SO" | sort -u > "$WORK/sostrings"
grep -xF -f "$WORK/javanames" "$WORK/sostrings" | sort -u > "$WORK/contract"

cd "$(dirname "$VENDOR")"
for n in $(cat "$WORK/contract"); do
    for f in $(grep -rl "^\.method .*[ ]$n(" --include=*.smali "$(basename "$VENDOR")" 2>/dev/null); do
        line=$(grep -h "^\.method .*[ ]$n(" "$f" | head -1)
        mods=$(echo "$line" | sed "s/^\.method //;s/ *$n(.*//")
        sig=$(echo "$line" | sed "s/.*$n//")
        printf "%-46s %-34s %-24s %s\n" "$(echo "$f" | sed 's|^\./||;s|\.smali$||')" "$n" "$sig" "$mods"
    done
done | sort
