#!/bin/bash
set -euo pipefail

PACKAGE_DIR=""
PLUGIN_DIR=""
PLUGIN_CACHE_DIR=""
REPLACE_EXISTING=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -PackageDir)
            PACKAGE_DIR="${2:-}"
            shift 2
            ;;
        -PluginDir)
            PLUGIN_DIR="${2:-}"
            shift 2
            ;;
        -PluginCacheDir)
            PLUGIN_CACHE_DIR="${2:-}"
            shift 2
            ;;
        -ReplaceExisting)
            REPLACE_EXISTING=1
            shift
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$PLUGIN_DIR" ]]; then
    PLUGIN_DIR="$PACKAGE_DIR"
fi
if [[ -z "$PLUGIN_DIR" ]]; then
    echo "PluginDir is required" >&2
    exit 2
fi

APP_SUPPORT_DIR="$HOME/Library/Application Support/OrcaSlicer/macos-bridge"
LOCAL_LIMA_ROOT="$APP_SUPPORT_DIR/lima"
LOCAL_LIMA_BIN="$LOCAL_LIMA_ROOT/bin"
RUNTIME_DIR="${PJARCZAK_MAC_RUNTIME_DIR:-$APP_SUPPORT_DIR/runtime}"
mkdir -p "$APP_SUPPORT_DIR" "$LOCAL_LIMA_ROOT" "$RUNTIME_DIR"

trim_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        return 1
    fi
    LC_ALL=C tr -d '\r' < "$path" | head -n 1 | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'
}

find_limactl() {
    if [[ -n "${PJARCZAK_LIMACTL:-}" && -x "${PJARCZAK_LIMACTL}" ]]; then
        printf '%s
' "$PJARCZAK_LIMACTL"
        return 0
    fi
    if command -v limactl >/dev/null 2>&1; then
        command -v limactl
        return 0
    fi
    if [[ -x "$LOCAL_LIMA_BIN/limactl" ]]; then
        printf '%s
' "$LOCAL_LIMA_BIN/limactl"
        return 0
    fi
    for candidate in /opt/homebrew/bin/limactl /usr/local/bin/limactl; do
        if [[ -x "$candidate" ]]; then
            printf '%s
' "$candidate"
            return 0
        fi
    done
    return 1
}

resolve_lima_version_from_redirect() {
    local effective_url=""
    effective_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' https://github.com/lima-vm/lima/releases/latest || true)
    case "$effective_url" in
        */tag/*)
            printf '%s
' "${effective_url##*/}"
            return 0
            ;;
    esac
    return 1
}

resolve_lima_version() {
    if [[ -n "${PJARCZAK_LIMA_VERSION:-}" ]]; then
        printf '%s
' "$PJARCZAK_LIMA_VERSION"
        return 0
    fi

    local version=""
    version=$(curl -fsSL https://api.github.com/repos/lima-vm/lima/releases/latest | awk -F'"' '/"tag_name"[[:space:]]*:/ { print $4; exit }' || true)
    if [[ -n "$version" ]]; then
        printf '%s
' "$version"
        return 0
    fi

    resolve_lima_version_from_redirect
}

install_lima_binary_locally() {
    local version
    version=$(resolve_lima_version)
    if [[ -z "$version" ]]; then
        echo "failed to resolve latest Lima version from GitHub API" >&2
        return 1
    fi

    local host_arch
    host_arch=$(uname -m)
    case "$host_arch" in
        arm64|aarch64)
            host_arch=arm64
            ;;
        x86_64|amd64)
            host_arch=x86_64
            ;;
        *)
            echo "unsupported macOS architecture for Lima: $host_arch" >&2
            return 1
            ;;
    esac

    local version_no_v="${version#v}"
    local base_url="https://github.com/lima-vm/lima/releases/download/${version}"
    local main_archive="lima-${version_no_v}-Darwin-${host_arch}.tar.gz"
    local guest_archive="lima-additional-guestagents-${version_no_v}-Darwin-${host_arch}.tar.gz"
    local tmpdir
    tmpdir=$(mktemp -d)
    trap 'rm -rf "$tmpdir"' RETURN

    curl -fL --retry 3 --retry-delay 2 "$base_url/$main_archive" -o "$tmpdir/$main_archive"
    tar -xzf "$tmpdir/$main_archive" -C "$LOCAL_LIMA_ROOT"

    if curl -fL --retry 3 --retry-delay 2 "$base_url/$guest_archive" -o "$tmpdir/$guest_archive"; then
        tar -xzf "$tmpdir/$guest_archive" -C "$LOCAL_LIMA_ROOT"
    fi

    [[ -x "$LOCAL_LIMA_BIN/limactl" ]]
}

ensure_lima_installed() {
    LIMACTL=$(find_limactl || true)
    if [[ -n "$LIMACTL" ]]; then
        return 0
    fi

    if command -v brew >/dev/null 2>&1; then
        brew install lima
        LIMACTL=$(find_limactl || true)
        if [[ -n "$LIMACTL" ]]; then
            return 0
        fi
    fi

    install_lima_binary_locally
    LIMACTL=$(find_limactl || true)
    [[ -n "$LIMACTL" ]]
}

maybe_install_rosetta() {
    if [[ "$(uname -m)" != "arm64" ]]; then
        return 0
    fi
    if pgrep -q oahd >/dev/null 2>&1; then
        return 0
    fi
    /usr/sbin/softwareupdate --install-rosetta --agree-to-license >/dev/null 2>&1 || true
}

copy_runtime_payload() {
    local src_dir="$1"
    local dst_dir="$2"
    local file
    local required_files=(
        libbambu_networking.so
        libBambuSource.so
        pjarczak_bambu_linux_host
        pjarczak_bambu_linux_host_abi1
        pjarczak_bambu_linux_host_abi0
        ca-certificates.crt
        slicer_base64.cer
    )

    for file in "${required_files[@]}"; do
        if [[ ! -f "$src_dir/$file" ]]; then
            echo "missing required runtime payload file: $file" >&2
            exit 1
        fi
        cp -f "$src_dir/$file" "$dst_dir/$file"
    done

    for file in liblive555.so libagora_rtc_sdk.so libagora-fdkaac.so; do
        if [[ -f "$src_dir/$file" ]]; then
            cp -f "$src_dir/$file" "$dst_dir/$file"
        fi
    done

    chmod 755 "$dst_dir/pjarczak_bambu_linux_host" "$dst_dir/pjarczak_bambu_linux_host_abi1" "$dst_dir/pjarczak_bambu_linux_host_abi0"
}

INSTANCE="${PJARCZAK_MAC_LIMA_INSTANCE:-}"
if [[ -z "$INSTANCE" ]]; then
    INSTANCE=$(trim_file "$PLUGIN_DIR/pjarczak_lima_instance.txt" || true)
fi
if [[ -z "$INSTANCE" ]]; then
    INSTANCE="orcaslicer-bambu-network"
fi

copy_runtime_payload "$PLUGIN_DIR" "$RUNTIME_DIR"
ensure_lima_installed
maybe_install_rosetta

START_ARGS=(start "--name=${INSTANCE}" --tty=false --mount-writable)
MACOS_MAJOR=$(sw_vers -productVersion | awk -F. '{print $1}')
if [[ "$MACOS_MAJOR" -ge 13 ]]; then
    START_ARGS+=(--vm-type=vz --network=vzNAT)
    if [[ "$(uname -m)" == "arm64" ]]; then
        START_ARGS+=(--rosetta)
    fi
fi

if [[ "$REPLACE_EXISTING" -eq 1 ]]; then
    "$LIMACTL" stop "$INSTANCE" >/dev/null 2>&1 || true
fi

if ! "$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1; then
    "$LIMACTL" "${START_ARGS[@]}" template:default
fi

"$LIMACTL" start-at-login "$INSTANCE" --enabled >/dev/null 2>&1 || true
"$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null
printf 'runtime installed
'
