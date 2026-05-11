#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
RUNTIME_ROOT="$PROJECT_DIR/tools/pjarczak_bambu_linux_host/runtime/linux-x86_64"
PROPRIETARY_DIR="$PROJECT_DIR/src/slic3r/Utils/PJarczakLinuxBridge/runtime/proprietary"

find_host_bin() {
    local name="$1"
    local candidate=""
    for candidate in \
        "$PROJECT_DIR/build/src/Release/$name" \
        "$PROJECT_DIR/build/$name" \
        "$PROJECT_DIR/build/src/$name"
    do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    find "$PROJECT_DIR/build" -type f -name "$name" 2>/dev/null | head -n 1
}

find_optional_file() {
    local name="$1"
    local candidate=""
    for candidate in \
        "$PROPRIETARY_DIR/$name" \
        "$PROJECT_DIR/tools/pjarczak_bambu_linux_host/runtime/linux-x86_64/$name" \
        "$PROJECT_DIR/build/$name" \
        "$PROJECT_DIR/$name"
    do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

collect_runtime_libs() {
    local host_bin="$1"
    ldd "$host_bin" | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^\// { print $1 }
    ' | sort -u
}

copy_runtime_libs() {
    local host_abi1="$1"
    local host_abi0="$2"
    local network_so="$3"
    local source_so="$4"
    mkdir -p "$RUNTIME_ROOT"
    mapfile -t libs < <({
        collect_runtime_libs "$host_abi1"
        collect_runtime_libs "$host_abi0"
        [[ -n "$network_so" ]] && collect_runtime_libs "$network_so"
        [[ -n "$source_so" ]] && collect_runtime_libs "$source_so"
    } | sort -u)
    local lib base
    for lib in "${libs[@]}"; do
        base="$(basename -- "$lib")"
        case "$base" in
            ld-linux*|ld-musl-*.so*|linux-vdso.so.*|libc.so.*|libm.so.*|libpthread.so.*|librt.so.*|libdl.so.*|libresolv.so.*|libnss_*.so*|libnsl.so.*|libutil.so.*|libanl.so.*)
                continue
                ;;
        esac
        cp -Lf "$lib" "$RUNTIME_ROOT/"
    done
}

write_manifest_if_payload_present() {
    local network_so="$1"
    local source_so="$2"
    if [[ -z "$network_so" || -z "$source_so" ]]; then
        return 0
    fi

    local abi_version
    abi_version="$(python3 - <<'PY' "$PROJECT_DIR"
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
text = (root / 'src' / 'slic3r' / 'Utils' / 'bambu_networking.hpp').read_text(encoding='utf-8', errors='ignore')
m = re.search(r'BAMBU_NETWORK_AGENT_VERSION\s+"([^"]+)"', text)
print(m.group(1) if m else '')
PY
)"

    if [[ -z "$abi_version" ]]; then
        echo "Could not extract BAMBU_NETWORK_AGENT_VERSION" >&2
        exit 1
    fi

    python3 - <<'PY' "$RUNTIME_ROOT" "$abi_version"
import hashlib, json, pathlib, sys
out_dir = pathlib.Path(sys.argv[1])
abi_version = sys.argv[2]
entries = []
for name in ("libbambu_networking.so", "libBambuSource.so"):
    path = out_dir / name
    if not path.exists():
        continue
    entry = {"name": name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    if name == "libbambu_networking.so":
        entry["abi_version"] = abi_version
    entries.append(entry)
(out_dir / "linux_payload_manifest.json").write_text(json.dumps({"files": entries}, indent=2) + "\n", encoding="utf-8")
PY
}

if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "this packaging script currently produces linux-x86_64 runtime only" >&2
    exit 1
fi

HOST_ABI1="$(find_host_bin pjarczak_bambu_linux_host_abi1 || true)"
HOST_ABI0="$(find_host_bin pjarczak_bambu_linux_host_abi0 || true)"
NETWORK_SO="$(find_optional_file libbambu_networking.so || true)"
SOURCE_SO="$(find_optional_file libBambuSource.so || true)"
if [[ -z "$HOST_ABI1" || ! -f "$HOST_ABI1" || -z "$HOST_ABI0" || ! -f "$HOST_ABI0" ]]; then
    echo "failed to find built pjarczak_bambu_linux_host_abi1/abi0 under $PROJECT_DIR/build" >&2
    echo "build them first in the full Orca Linux build context, for example:" >&2
    echo "  cmake --build build --config Release --target pjarczak_bambu_linux_host" >&2
    exit 1
fi

rm -rf "$RUNTIME_ROOT"
mkdir -p "$RUNTIME_ROOT"

cp -f "$PROJECT_DIR/tools/pjarczak_bambu_runtime/wsl/pjarczak_bambu_linux_host" "$RUNTIME_ROOT/pjarczak_bambu_linux_host"
cp -f "$HOST_ABI1" "$RUNTIME_ROOT/pjarczak_bambu_linux_host_abi1"
cp -f "$HOST_ABI0" "$RUNTIME_ROOT/pjarczak_bambu_linux_host_abi0"
chmod +x "$RUNTIME_ROOT/pjarczak_bambu_linux_host" "$RUNTIME_ROOT/pjarczak_bambu_linux_host_abi1" "$RUNTIME_ROOT/pjarczak_bambu_linux_host_abi0"

for extra in \
    "$PROJECT_DIR/cert/ca-certificates.crt" \
    "$PROJECT_DIR/cert/slicer_base64.cer" \
    "$PROJECT_DIR/resources/cert/ca-certificates.crt" \
    "$PROJECT_DIR/resources/cert/slicer_base64.cer"; do
    if [[ -f "$extra" ]]; then
        cp -f "$extra" "$RUNTIME_ROOT/$(basename -- "$extra")"
    fi
done

if [[ -n "$NETWORK_SO" ]]; then
    cp -f "$NETWORK_SO" "$RUNTIME_ROOT/libbambu_networking.so"
fi
if [[ -n "$SOURCE_SO" ]]; then
    cp -f "$SOURCE_SO" "$RUNTIME_ROOT/libBambuSource.so"
fi

copy_runtime_libs "$HOST_ABI1" "$HOST_ABI0" "$NETWORK_SO" "$SOURCE_SO"
write_manifest_if_payload_present "$NETWORK_SO" "$SOURCE_SO"

echo "linux host runtime packaged into:"
echo "  $RUNTIME_ROOT"
find "$RUNTIME_ROOT" -maxdepth 1 -type f | sort
