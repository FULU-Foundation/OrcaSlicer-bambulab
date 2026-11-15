#!/bin/sh
# Install the Linux arm64 bridge wrapper into OrcaSlicer's plug-in directory.
set -eu

DIR=${1:-"$HOME/.config/OrcaSlicer/plugins"}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

mkdir -p "$DIR"
install -m755 "$HERE/pjarczak_bambu_linux_host" "$DIR/"
install -m755 "$HERE/bridge_relay.py" "$DIR/"
install -m755 "$HERE/bridge_exec.py" "$DIR/"

echo "Installed the bridge wrapper into $DIR"
echo
echo "Remaining steps:"
echo "  1. build the x86-64 host:        $HERE/build-host.sh"
echo "     and copy pjarczak_bambu_linux_host_abi1/_abi0 into $DIR"
echo "  2. build the FEX rootfs:         $HERE/build-rootfs.sh"
echo "  3. install the arm64 bridge .so as $DIR/libbambu_networking.so"
echo "     (build target: pjarczak_bambu_networking_bridge)"
echo "  4. let OrcaSlicer download the plug-in, then move"
echo "     libbambu_networking.so, libBambuSource.so, libagora*.so and"
echo "     liblive555.so from $DIR into $DIR/runtime/"
echo "  5. launch with:"
echo "     PJARCZAK_LINUX_BRIDGE_ENABLED=1 \\"
echo "     PJARCZAK_LINUX_BRIDGE_USE_NETWORK_MODULE=1 \\"
echo "     orca-slicer"
