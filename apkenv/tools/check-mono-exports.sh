#!/bin/bash
# check-mono-exports.sh <libmono.so> - Checkpoint B gate.
# Exits 0 iff every mono_* symbol libunity.so imports is exported by <libmono.so>.
set -e
# Resolve arguments against the CALLER's cwd before we move, so both
# "tools/check-mono-exports.sh apkenv/hostlibs/..." (from the repo root) and a
# bare/absolute path work.
abspath() { case "$1" in /*) printf '%s\n' "$1";; *) printf '%s\n' "$PWD/$1";; esac; }
[ -n "$1" ] && set -- "$(abspath "$1")" "${@:2}"
[ -n "$2" ] && set -- "$1" "$(abspath "$2")"
cd "$(dirname "$0")/.."
LIB="${1:-hostlibs/webos/libmono-webos.so}"
WANT="${2:-../plan/tr2-mono-imports.txt}"
NM=/opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-nm
[ -f "$LIB" ]  || { echo "no such library: $LIB" >&2; exit 2; }
[ -f "$WANT" ] || { echo "no import list: $WANT" >&2; exit 2; }
have=$(mktemp); want=$(mktemp); trap 'rm -f "$have" "$want"' EXIT
$NM -D --defined-only "$LIB" | awk '{print $3}' | sort -u > "$have"
sort -u "$WANT" > "$want"
missing=$(comm -23 "$want" "$have")
n_want=$(wc -l < "$want"); n_have=$(wc -l < "$have")
if [ -n "$missing" ]; then
    echo "FAIL: $(echo "$missing" | wc -l)/$n_want libunity->libmono bridge symbols MISSING from $LIB:"
    echo "$missing" | sed 's/^/  /'
    exit 1
fi
echo "OK: all $n_want libunity->libmono bridge symbols exported by $LIB ($n_have exports total)"
