#!/bin/sh
# Build the FEX Ubuntu 24.04 rootfs that the bridge host runs in.
#
# The stock FEX rootfs has no GTK/WebKit, which the Bambu plug-in needs for its
# login webview.  This script extracts the official image, installs WebKitGTK
# into it with an amd64 container chroot, and repacks it as an erofs image.
#
# Requires: erofs-utils (fsck.erofs, mkfs.erofs), podman, curl.
set -eu

ERO_URL=${1:-https://rootfs.fex-emu.gg/Ubuntu_24_04/2026-08-11/Ubuntu_24_04.ero}
OUT=${2:-"$HOME/.local/opt/orcaslicer-fex/ubuntu.erofs"}
WORK=${TMPDIR:-/tmp}/pjarczak-rootfs
ROOTFS="$WORK/rootfs"

mkdir -p "$WORK"
if [ ! -f "$WORK/rootfs.ero" ]; then
    echo "Downloading $ERO_URL"
    curl -L -o "$WORK/rootfs.ero" "$ERO_URL"
fi

echo "Extracting rootfs"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"
fsck.erofs --extract="$ROOTFS" "$WORK/rootfs.ero"

# chroot hygiene: the FEX image ships without /tmp, /dev, /proc, passwd/group.
mkdir -p "$ROOTFS/tmp" "$ROOTFS/var/tmp" "$ROOTFS/proc" "$ROOTFS/sys" "$ROOTFS/dev"
chmod 1777 "$ROOTFS/tmp" "$ROOTFS/var/tmp"
cp -L /etc/resolv.conf "$ROOTFS/etc/resolv.conf" 2>/dev/null || true
if [ ! -s "$ROOTFS/etc/passwd" ]; then
    cp "$ROOTFS/usr/share/base-passwd/passwd.master" "$ROOTFS/etc/passwd"
fi
if [ ! -s "$ROOTFS/etc/group" ]; then
    cp "$ROOTFS/usr/share/base-passwd/group.master" "$ROOTFS/etc/group"
fi
grep -q '^messagebus:' "$ROOTFS/etc/group" || echo 'messagebus:x:102:' >> "$ROOTFS/etc/group"
# The FEX image may carry a third-party Mesa PPA that is not needed here.
sed -i '/kisak/d' "$ROOTFS"/etc/apt/sources.list "$ROOTFS"/etc/apt/sources.list.d/* 2>/dev/null || true

echo "Installing WebKitGTK into the rootfs"
podman run --rm --arch amd64 --security-opt label=disable \
    -v "$ROOTFS":/rootfs -v /dev:/rootfs/dev -v /proc:/rootfs/proc -v /sys:/rootfs/sys \
    docker.io/library/ubuntu:24.04 \
    bash -lc 'chroot /rootfs /bin/bash -c "export DEBIAN_FRONTEND=noninteractive; apt-get update && apt-get install -y libwebkit2gtk-4.1-0 libjavascriptcoregtk-4.1-0 libsoup-3.0-0 ca-certificates"'

echo "Packing $OUT"
mkdir -p "$(dirname "$OUT")"
mkfs.erofs -zlz4hc -b 4096 -T 1 "$OUT" "$ROOTFS"

echo "Done: $OUT"
