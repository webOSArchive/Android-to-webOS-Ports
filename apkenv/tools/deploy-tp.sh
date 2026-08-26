#!/bin/sh
# deploy-tp.sh — copy a file to the HP TouchPad over its legacy-cipher SSH.
# scp's legacy protocol fails against the device; piping through `cat` works.
#   usage: apkenv/tools/deploy-tp.sh <local-file> <device-path> [host]
set -e
src="$1"; dst="$2"; host="${3:-192.168.10.88}"
[ -f "$src" ] || { echo "no such file: $src" >&2; exit 1; }
SSH="ssh -oConnectTimeout=8 -oKexAlgorithms=+diffie-hellman-group1-sha1 \
 -oCiphers=+aes128-cbc,3des-cbc -oHostKeyAlgorithms=+ssh-rsa,ssh-dss \
 -oPubkeyAcceptedAlgorithms=+ssh-rsa,ssh-dss -oMACs=+hmac-sha1 -i $HOME/.ssh/id_rsa root@$host"
$SSH "cat > '$dst.new' && chmod +x '$dst.new' && mv '$dst.new' '$dst' && md5sum '$dst'" < "$src"
echo "local: $(md5sum "$src")"
