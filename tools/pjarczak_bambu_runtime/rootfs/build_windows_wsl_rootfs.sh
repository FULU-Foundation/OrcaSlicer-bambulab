#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUTPUT_TAR="${1:-$SCRIPT_DIR/windows-wsl2-rootfs.tar}"
BASE_IMAGE="${PJARCZAK_WSL_ROOTFS_IMAGE:-ubuntu:24.04}"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found. Install Docker or provide a prebuilt windows-wsl2-rootfs.tar." >&2
    exit 1
fi

mkdir -p "$(dirname -- "$OUTPUT_TAR")"

CONTAINER_NAME="pjarczak-bambu-rootfs-$(date +%s)-$$"
cleanup() {
    docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker pull "$BASE_IMAGE" >/dev/null
docker create --name "$CONTAINER_NAME" "$BASE_IMAGE" /bin/sh -lc 'exit 0' >/dev/null

rm -f "$OUTPUT_TAR"
docker export "$CONTAINER_NAME" -o "$OUTPUT_TAR"

if [[ ! -s "$OUTPUT_TAR" ]]; then
    echo "failed to create rootfs tar: $OUTPUT_TAR" >&2
    exit 1
fi

echo "WSL rootfs created:"
echo "  $OUTPUT_TAR"
