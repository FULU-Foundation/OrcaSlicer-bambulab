#!/bin/bash

set -e
set -o pipefail
SECONDS=0

while getopts ":dpa:snt:xbc:i:1Tuh" opt; do
  case "${opt}" in
    d )
        export BUILD_TARGET="deps"
        ;;
    p )
        export PACK_DEPS="1"
        ;;
    a )
        export ARCH="$OPTARG"
        ;;
    s )
        export BUILD_TARGET="slicer"
        ;;
    n )
        export NIGHTLY_BUILD="1"
        ;;
    t )
        export OSX_DEPLOYMENT_TARGET="$OPTARG"
        ;;
    x )
        export SLICER_CMAKE_GENERATOR="Ninja Multi-Config"
        export SLICER_BUILD_TARGET="all"
        export DEPS_CMAKE_GENERATOR="Ninja"
        ;;
    b )
        export BUILD_ONLY="1"
        ;;
    c )
        export BUILD_CONFIG="$OPTARG"
        ;;
    i )
        export CMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH:+$CMAKE_IGNORE_PREFIX_PATH;}$OPTARG"
        ;;
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        ;;
    T )
        export BUILD_TESTS="1"
        ;;
    u )
        export BUILD_TARGET="universal"
        ;;
    h ) echo "Usage: ./build_release_macos.sh [-d]"
        echo "   -d: Build deps only"
        echo "   -a: Set ARCHITECTURE (arm64 or x86_64 or universal)"
        echo "   -s: Build slicer only"
        echo "   -u: Build universal app only (requires existing arm64 and x86_64 app bundles)"
        echo "   -n: Nightly build"
        echo "   -t: Specify minimum version of the target platform, default is 11.3"
        echo "   -x: Use Ninja Multi-Config CMake generator, default is Xcode"
        echo "   -b: Build without reconfiguring CMake"
        echo "   -c: Set CMake build configuration, default is Release"
        echo "   -i: Add a prefix to ignore during CMake dependency discovery (repeatable), defaults to /opt/local:/usr/local:/opt/homebrew"
        echo "   -1: Use single job for building"
        echo "   -T: Build and run tests"
        exit 0
        ;;
    * )
        ;;
  esac
done

if [ -z "$ARCH" ]; then
    ARCH="$(uname -m)"
    export ARCH
fi

if [ -z "$BUILD_CONFIG" ]; then
  export BUILD_CONFIG="Release"
fi

if [ -z "$BUILD_TARGET" ]; then
  export BUILD_TARGET="all"
fi

if [ -z "$SLICER_CMAKE_GENERATOR" ]; then
  export SLICER_CMAKE_GENERATOR="Xcode"
fi

if [ -z "$SLICER_BUILD_TARGET" ]; then
  export SLICER_BUILD_TARGET="ALL_BUILD"
fi

if [ -z "$DEPS_CMAKE_GENERATOR" ]; then
  export DEPS_CMAKE_GENERATOR="Unix Makefiles"
fi

if [ -z "$OSX_DEPLOYMENT_TARGET" ]; then
  export OSX_DEPLOYMENT_TARGET="11.3"
fi

if [ -z "$CMAKE_IGNORE_PREFIX_PATH" ]; then
  export CMAKE_IGNORE_PREFIX_PATH="/opt/local:/usr/local:/opt/homebrew"
fi

CMAKE_VERSION=$(cmake --version | head -1 | sed -E 's/[^0-9]*([0-9]+).*/\1/')
if [ "$CMAKE_VERSION" -ge 4 ] 2>/dev/null; then
  export CMAKE_POLICY_VERSION_MINIMUM=3.5
  export CMAKE_POLICY_COMPAT="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
  echo "Detected CMake 4.x, adding compatibility flag (env + cmake arg)"
else
  export CMAKE_POLICY_COMPAT=""
fi

echo "Build params:"
echo " - ARCH: $ARCH"
echo " - BUILD_CONFIG: $BUILD_CONFIG"
echo " - BUILD_TARGET: $BUILD_TARGET"
echo " - CMAKE_GENERATOR: $SLICER_CMAKE_GENERATOR for Slicer, $DEPS_CMAKE_GENERATOR for deps"
echo " - OSX_DEPLOYMENT_TARGET: $OSX_DEPLOYMENT_TARGET"
echo " - CMAKE_IGNORE_PREFIX_PATH: $CMAKE_IGNORE_PREFIX_PATH"
echo

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_BUILD_DIR="$PROJECT_DIR/build/$ARCH"
DEPS_DIR="$PROJECT_DIR/deps"
HOST_RUNTIME_DIR="$PROJECT_DIR/tools/pjarczak_bambu_linux_host/runtime/linux-x86_64"
HOST_WRAPPER="$PROJECT_DIR/tools/pjarczak_bambu_linux_host/pjarczak-bambu-linux-host-wrapper"
MAC_RUNTIME_HELPERS_DIR="$PROJECT_DIR/tools/pjarczak_bambu_runtime/macos"

export BUILD_DIR_CONFIG_SUBDIR="/$BUILD_CONFIG"

copy_linux_bridge_runtime_to_app() {
    local app_path="$1"
    local install_root="$2"
    local macos_dir="$app_path/Contents/MacOS"
    local bridge_dylib="$install_root/libpjarczak_bambu_networking_bridge.dylib"

    if [ ! -f "$HOST_RUNTIME_DIR/pjarczak_bambu_linux_host" ]; then
        echo "Missing linux host runtime: $HOST_RUNTIME_DIR/pjarczak_bambu_linux_host"
        echo "Build it first on Linux with:"
        echo "  tools/pjarczak_bambu_linux_host/package_linux_host_runtime.sh"
        exit 1
    fi

    if [ ! -f "$HOST_RUNTIME_DIR/pjarczak_bambu_linux_host_abi1" ]; then
        echo "Missing linux host ABI1 runtime: $HOST_RUNTIME_DIR/pjarczak_bambu_linux_host_abi1"
        exit 1
    fi

    if [ ! -f "$HOST_RUNTIME_DIR/pjarczak_bambu_linux_host_abi0" ]; then
        echo "Missing linux host ABI0 runtime: $HOST_RUNTIME_DIR/pjarczak_bambu_linux_host_abi0"
        exit 1
    fi

    if [ ! -f "$HOST_WRAPPER" ]; then
        echo "Missing mac wrapper: $HOST_WRAPPER"
        exit 1
    fi

    if [ ! -f "$MAC_RUNTIME_HELPERS_DIR/pjarczak_install_macos_runtime.sh" ]; then
        echo "Missing mac runtime installer: $MAC_RUNTIME_HELPERS_DIR/pjarczak_install_macos_runtime.sh"
        exit 1
    fi

    if [ ! -f "$MAC_RUNTIME_HELPERS_DIR/pjarczak_verify_macos_runtime.sh" ]; then
        echo "Missing mac runtime verifier: $MAC_RUNTIME_HELPERS_DIR/pjarczak_verify_macos_runtime.sh"
        exit 1
    fi

    if [ ! -f "$MAC_RUNTIME_HELPERS_DIR/pjarczak_lima_instance.txt" ]; then
        echo "Missing Lima instance file: $MAC_RUNTIME_HELPERS_DIR/pjarczak_lima_instance.txt"
        exit 1
    fi

    if [ ! -f "$bridge_dylib" ]; then
        echo "Missing installed bridge dylib: $bridge_dylib"
        exit 1
    fi

    cp -f "$bridge_dylib" "$macos_dir/"
    find "$HOST_RUNTIME_DIR" -maxdepth 1 -type f -exec cp -f {} "$macos_dir/" \;
    cp -f "$HOST_WRAPPER" "$macos_dir/"
    cp -f "$MAC_RUNTIME_HELPERS_DIR/pjarczak_install_macos_runtime.sh" "$macos_dir/install_runtime_macos.sh"
    cp -f "$MAC_RUNTIME_HELPERS_DIR/pjarczak_verify_macos_runtime.sh" "$macos_dir/verify_runtime_macos.sh"
    cp -f "$MAC_RUNTIME_HELPERS_DIR/pjarczak_lima_instance.txt" "$macos_dir/"
    if [ -f "$MAC_RUNTIME_HELPERS_DIR/README_runtime_bridge.txt" ]; then
        cp -f "$MAC_RUNTIME_HELPERS_DIR/README_runtime_bridge.txt" "$macos_dir/"
    fi

    chmod +x "$macos_dir/pjarczak_bambu_linux_host"
    chmod +x "$macos_dir/pjarczak-bambu-linux-host-wrapper"
    chmod +x "$macos_dir/install_runtime_macos.sh"
    chmod +x "$macos_dir/verify_runtime_macos.sh"
}

build_deps() {
    for _ARCH in x86_64 arm64; do
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then
            PROJECT_BUILD_DIR="$PROJECT_DIR/build/$_ARCH"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building deps..."
            (
                set -x
                mkdir -p "$DEPS"
                cd "$DEPS_BUILD_DIR"
                if [ "1." != "$BUILD_ONLY". ]; then
                    cmake "${DEPS_DIR}"                         -G "${DEPS_CMAKE_GENERATOR}"                         -DCMAKE_BUILD_TYPE="$BUILD_CONFIG"                         -DCMAKE_OSX_ARCHITECTURES:STRING="${_ARCH}"                         -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}"                         -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}"                         ${CMAKE_POLICY_COMPAT}
                fi
                cmake --build . --config "$BUILD_CONFIG" --target deps
            )
        fi
    done
}

pack_deps() {
    echo "Packing deps..."
    (
        set -x
        cd "$DEPS_DIR"
        tar -zcvf "OrcaSlicer_dep_mac_${ARCH}_$(date +"%Y%m%d").tar.gz" "build"
    )
}

build_slicer() {
    for _ARCH in x86_64 arm64; do
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then
            PROJECT_BUILD_DIR="$PROJECT_DIR/build/$_ARCH"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building slicer for $_ARCH..."
            (
                set -x
                mkdir -p "$PROJECT_BUILD_DIR"
                cd "$PROJECT_BUILD_DIR"
                if [ "1." != "$BUILD_ONLY". ]; then
                    cmake "${PROJECT_DIR}"                         -G "${SLICER_CMAKE_GENERATOR}"                         -DORCA_TOOLS=ON                         ${ORCA_UPDATER_SIG_KEY:+-DORCA_UPDATER_SIG_KEY="$ORCA_UPDATER_SIG_KEY"}                         ${BUILD_TESTS:+-DBUILD_TESTS=ON}                         -DCMAKE_BUILD_TYPE="$BUILD_CONFIG"                         -DCMAKE_OSX_ARCHITECTURES="${_ARCH}"                         -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}"                         -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}"                         ${CMAKE_POLICY_COMPAT}
                fi
                cmake --build . --config "$BUILD_CONFIG" --target "$SLICER_BUILD_TARGET"
                cmake --install . --config "$BUILD_CONFIG"
            )

            if [ "1." == "$BUILD_TESTS". ]; then
                echo "Running tests for $_ARCH..."
                (
                    set -x
                    cd "$PROJECT_BUILD_DIR"
                    ctest --build-config "$BUILD_CONFIG" --output-on-failure
                )
            fi

            echo "Verify localization with gettext..."
            (
                cd "$PROJECT_DIR"
                ./scripts/run_gettext.sh
            )

            echo "Fix macOS app package..."
            (
                cd "$PROJECT_BUILD_DIR"
                mkdir -p OrcaSlicer
                cd OrcaSlicer
                rm -rf ./OrcaSlicer.app
                cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer.app" ./OrcaSlicer.app

                resources_path=$(readlink ./OrcaSlicer.app/Contents/Resources)
                rm ./OrcaSlicer.app/Contents/Resources
                cp -R "$resources_path" ./OrcaSlicer.app/Contents/Resources

                find ./OrcaSlicer.app/ -name '.DS_Store' -delete

                if [ -f "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app/Contents/MacOS/OrcaSlicer_profile_validator" ]; then
                    echo "Copying OrcaSlicer_profile_validator.app..."
                    rm -rf ./OrcaSlicer_profile_validator.app
                    cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app" ./OrcaSlicer_profile_validator.app
                    find ./OrcaSlicer_profile_validator.app/ -name '.DS_Store' -delete
                fi

                copy_linux_bridge_runtime_to_app "./OrcaSlicer.app" "$PROJECT_BUILD_DIR/OrcaSlicer"
            )
        fi
    done
}

lipo_dir() {
    local universal_dir="$1"
    local x86_64_dir="$2"

    while IFS= read -r -d '' f; do
        local rel="${f#"$universal_dir"/}"
        local x86="$x86_64_dir/$rel"
        if [ -f "$x86" ]; then
            echo "  lipo: $rel"
            lipo -create "$f" "$x86" -output "$f.tmp"
            mv "$f.tmp" "$f"
        else
            echo "  warning: no x86_64 counterpart for $rel, keeping arm64 only"
        fi
    done < <(find "$universal_dir" -type f -print0 | while IFS= read -r -d '' candidate; do
        if file "$candidate" | grep -q "Mach-O"; then
            printf '%s\0' "$candidate"
        fi
    done)
}

build_universal() {
    echo "Building universal binary..."

    PROJECT_BUILD_DIR="$PROJECT_DIR/build/$ARCH"
    ARM64_APP="$PROJECT_DIR/build/arm64/OrcaSlicer/OrcaSlicer.app"
    X86_64_APP="$PROJECT_DIR/build/x86_64/OrcaSlicer/OrcaSlicer.app"

    mkdir -p "$PROJECT_BUILD_DIR/OrcaSlicer"
    UNIVERSAL_APP="$PROJECT_BUILD_DIR/OrcaSlicer/OrcaSlicer.app"
    rm -rf "$UNIVERSAL_APP"
    cp -R "$ARM64_APP" "$UNIVERSAL_APP"

    echo "Creating universal binaries for OrcaSlicer.app..."
    lipo_dir "$UNIVERSAL_APP" "$X86_64_APP"
    echo "Universal OrcaSlicer.app created at $UNIVERSAL_APP"

    ARM64_VALIDATOR="$PROJECT_DIR/build/arm64/OrcaSlicer/OrcaSlicer_profile_validator.app"
    X86_64_VALIDATOR="$PROJECT_DIR/build/x86_64/OrcaSlicer/OrcaSlicer_profile_validator.app"
    if [ -d "$ARM64_VALIDATOR" ] && [ -d "$X86_64_VALIDATOR" ]; then
        echo "Creating universal binaries for OrcaSlicer_profile_validator.app..."
        UNIVERSAL_VALIDATOR_APP="$PROJECT_BUILD_DIR/OrcaSlicer/OrcaSlicer_profile_validator.app"
        rm -rf "$UNIVERSAL_VALIDATOR_APP"
        cp -R "$ARM64_VALIDATOR" "$UNIVERSAL_VALIDATOR_APP"
        lipo_dir "$UNIVERSAL_VALIDATOR_APP" "$X86_64_VALIDATOR"
        echo "Universal OrcaSlicer_profile_validator.app created at $UNIVERSAL_VALIDATOR_APP"
    fi
}

case "${BUILD_TARGET}" in
    all)
        build_deps
        build_slicer
        ;;
    deps)
        build_deps
        ;;
    slicer)
        build_slicer
        ;;
    universal)
        build_universal
        ;;
    *)
        echo "Unknown target: $BUILD_TARGET. Available targets: deps, slicer, universal, all."
        exit 1
        ;;
esac

if [ "$ARCH" = "universal" ] && { [ "$BUILD_TARGET" = "all" ] || [ "$BUILD_TARGET" = "slicer" ]; }; then
    build_universal
fi

if [ "1." == "$PACK_DEPS". ]; then
    pack_deps
fi

elapsed=$SECONDS
printf "
Build completed in %dh %dm %ds
" $((elapsed/3600)) $((elapsed%3600/60)) $((elapsed%60))
