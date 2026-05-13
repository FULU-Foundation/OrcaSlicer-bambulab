#!/bin/bash
#
# Package an already-built OrcaSlicer.app into an unsigned DMG.
#
# Prerequisite: build_release_macos.sh has produced OrcaSlicer.app at
#               build/<arch>/OrcaSlicer/OrcaSlicer.app
#
# Usage:
#   ./package_macos_dmg.sh [-a <arch>] [-v <version>] [-o <output-dir>]
#
#   -a  Architecture: arm64 | x86_64 | universal (default: host arch)
#   -v  Version string used in the DMG filename (default: parsed from version.inc)
#   -o  Output directory for the .dmg (default: repository root)
#   -h  Show this help

set -e
set -o pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH=""
VERSION=""
OUTPUT_DIR="$PROJECT_DIR"

while getopts ":a:v:o:h" opt; do
    case "${opt}" in
        a) ARCH="$OPTARG" ;;
        v) VERSION="$OPTARG" ;;
        o) OUTPUT_DIR="$OPTARG" ;;
        h)
            sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) ;;
    esac
done

if [ -z "$ARCH" ]; then
    ARCH="$(uname -m)"
fi

case "$ARCH" in
    arm64|x86_64|universal) ;;
    *)
        echo "ERROR: unsupported arch '$ARCH' (expected arm64 | x86_64 | universal)" >&2
        exit 1
        ;;
esac

APP_DIR="$PROJECT_DIR/build/$ARCH/OrcaSlicer"
APP_BUNDLE="$APP_DIR/OrcaSlicer.app"

if [ ! -d "$APP_BUNDLE" ]; then
    echo "ERROR: OrcaSlicer.app not found at:" >&2
    echo "  $APP_BUNDLE" >&2
    echo "" >&2
    echo "Build it first, e.g.:" >&2
    echo "  ./build_release_macos.sh -a $ARCH" >&2
    exit 1
fi

if [ -z "$VERSION" ]; then
    VERSION="$(grep -E 'set\(SoftFever_VERSION' "$PROJECT_DIR/version.inc" \
                | head -n1 \
                | sed -E 's/.*"([^"]+)".*/\1/')"
fi

if [ -z "$VERSION" ]; then
    echo "ERROR: could not derive version from version.inc; pass it with -v" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

STAGING_DIR="$APP_DIR/OrcaSlicer_dmg"
DMG_NAME="OrcaSlicer-bambulab_Mac_${ARCH}_${VERSION}.dmg"
DMG_PATH="$OUTPUT_DIR/$DMG_NAME"

echo "Preparing DMG staging at $STAGING_DIR"
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

cp -R "$APP_BUNDLE" "$STAGING_DIR/"

VALIDATOR_BUNDLE="$APP_DIR/OrcaSlicer_profile_validator.app"
if [ -d "$VALIDATOR_BUNDLE" ]; then
    echo "Including OrcaSlicer_profile_validator.app"
    cp -R "$VALIDATOR_BUNDLE" "$STAGING_DIR/"
fi

ln -sfn /Applications "$STAGING_DIR/Applications"

find "$STAGING_DIR" -name '.DS_Store' -delete

# macOS 15+ adds com.apple.provenance xattr that breaks hdiutil with
# "Operation not permitted" when mounting the staging volume.
xattr -rcs "$STAGING_DIR" 2>/dev/null || true

rm -f "$DMG_PATH"

echo "Creating DMG: $DMG_PATH"
hdiutil create \
    -volname "OrcaSlicer" \
    -srcfolder "$STAGING_DIR" \
    -ov \
    -format UDZO \
    "$DMG_PATH"

rm -rf "$STAGING_DIR"

echo ""
echo "Done."
echo "  $DMG_PATH"
echo ""
echo "NOTE: this DMG is unsigned and not notarized."
echo "      First launch on another Mac will need: right-click -> Open,"
echo "      or:  xattr -dr com.apple.quarantine /Applications/OrcaSlicer.app"
