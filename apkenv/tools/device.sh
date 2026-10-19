# device.sh - sourced by every script that talks to a TouchPad.
#
# With one device attached, novacom/palm-* just use it. With several, they
# silently pick one - and on 2026-10-19 the dev pad and a family member's pad
# were attached together. So: DEVICE=<nduid or name> (from `novacom -l`)
# selects the target, and with more than one webOS device attached and no
# DEVICE, device_select refuses instead of guessing.
#
#   DEVICE=aa08217b5e79add3e74db145535ac066cfe1dcb3 tools/grab.sh
#
# After device_select, pass "${ND[@]}" to every novacom / palm-install /
# palm-launch call: `novacom "${ND[@]}" run ...`. Empty when DEVICE is unset.
# Android devices (novacom lists them as "adb") are not counted.

device_select() {
    ND=()
    if [ -n "$DEVICE" ]; then
        ND=(-d "$DEVICE")
        echo "== device: $DEVICE" >&2
        return 0
    fi
    local list n
    list=$(timeout --foreground 20 novacom -l < /dev/null 2>/dev/null | grep -v ' adb ')
    n=$(printf '%s\n' "$list" | grep -c .)
    if [ "$n" -gt 1 ]; then
        echo "ERROR: $n webOS devices attached; set DEVICE=<nduid> to choose one:" >&2
        printf '%s\n' "$list" | sed 's/^/  /' >&2
        exit 1
    fi
}
