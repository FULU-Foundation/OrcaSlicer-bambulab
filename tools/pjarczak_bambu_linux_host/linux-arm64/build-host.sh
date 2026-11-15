#!/bin/sh
# Build the x86-64 Bambu network plug-in host on an aarch64 machine.
#
# The host is a normal x86-64 ELF binary; it is built inside an amd64 podman
# container (qemu emulation) and then executed under FEX/muvm on the host.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
OUT=${1:-"$PWD/out"}
mkdir -p "$OUT"

podman run --rm --arch amd64 --security-opt label=disable \
    -v "$REPO":/w -v "$OUT":/out -w /w docker.io/library/ubuntu:24.04 \
    bash -lc 'apt-get update && apt-get install -y --no-install-recommends g++ cmake make && \
              cmake -S /w/tools/pjarczak_bambu_linux_host -B /out/build -DCMAKE_BUILD_TYPE=Release && \
              cmake --build /out/build -j"$(nproc)"'

echo
echo "Built:"
ls -l "$OUT/build/tools/pjarczak_bambu_linux_host/pjarczak_bambu_linux_host_abi1" \
      "$OUT/build/tools/pjarczak_bambu_linux_host/pjarczak_bambu_linux_host_abi0"
echo
echo "Copy them next to the bridge wrapper, for example:"
echo "  install -m755 $OUT/build/tools/pjarczak_bambu_linux_host/pjarczak_bambu_linux_host_abi1 ~/.config/OrcaSlicer/plugins/"
echo "  install -m755 $OUT/build/tools/pjarczak_bambu_linux_host/pjarczak_bambu_linux_host_abi0 ~/.config/OrcaSlicer/plugins/"
