#!/bin/sh
set -eu

log() {
    printf '%s\n' "[pjarczak_wsl_run_host] $*" >&2
}

MODE="run"
if [ "${1:-}" = "--probe" ]; then
    MODE="probe"
    shift
fi

PACKAGE_DIR="${1:-${PJARCZAK_BAMBU_WINDOWS_PLUGIN_DIR:-}}"
PLUGIN_CACHE_DIR="${2:-${PJARCZAK_BAMBU_WINDOWS_PLUGIN_CACHE_DIR:-}}"
if [ -z "$PACKAGE_DIR" ]; then
    log "missing Windows package directory path"
    exit 127
fi

if [ -n "$PLUGIN_CACHE_DIR" ] && [ ! -d "$PLUGIN_CACHE_DIR" ] && [ -d "$PLUGIN_CACHE_DIR/plugins" ]; then
    PLUGIN_CACHE_DIR="$PLUGIN_CACHE_DIR/plugins"
fi
if [ -n "$PLUGIN_CACHE_DIR" ] && [ -d "$PLUGIN_CACHE_DIR/plugins" ] && [ ! -f "$PLUGIN_CACHE_DIR/libbambu_networking.so" ] && [ ! -f "$PLUGIN_CACHE_DIR/libBambuSource.so" ]; then
    PLUGIN_CACHE_DIR="$PLUGIN_CACHE_DIR/plugins"
fi

HOST_SRC="$PACKAGE_DIR/pjarczak_bambu_linux_host"
if [ ! -f "$HOST_SRC" ]; then
    log "missing runtime file: $HOST_SRC"
    exit 127
fi

find_preferred_file() {
    name="$1"
    if [ -f "$PACKAGE_DIR/$name" ]; then
        printf '%s\n' "$PACKAGE_DIR/$name"
        return 0
    fi
    if [ -n "$PLUGIN_CACHE_DIR" ] && [ -f "$PLUGIN_CACHE_DIR/$name" ]; then
        printf '%s\n' "$PLUGIN_CACHE_DIR/$name"
        return 0
    fi
    return 1
}

resolve_payload_sources() {
    NETWORK_SRC="$(find_preferred_file libbambu_networking.so || true)"
    SOURCE_SRC="$(find_preferred_file libBambuSource.so || true)"
    ABI1_SRC="$(find_preferred_file pjarczak_bambu_linux_host_abi1 || true)"
    ABI0_SRC="$(find_preferred_file pjarczak_bambu_linux_host_abi0 || true)"
    CA_BUNDLE_SRC="$(find_preferred_file ca-certificates.crt || true)"
    SLICER_CERT_SRC="$(find_preferred_file slicer_base64.cer || true)"
    MANIFEST_SRC="$(find_preferred_file linux_payload_manifest.json || true)"
}

wait_for_payload_sources() {
    if [ "$MODE" = "probe" ]; then
        return 1
    fi
    WAIT_SECS="${PJARCZAK_BAMBU_PLUGIN_WAIT_SECS:-300}"
    SLEEP_SECS="${PJARCZAK_BAMBU_PLUGIN_WAIT_INTERVAL_SECS:-2}"
    DEADLINE=$(( $(date +%s) + WAIT_SECS ))
    while :; do
        resolve_payload_sources
        if [ -n "$NETWORK_SRC" ] && [ -n "$SOURCE_SRC" ]; then
            return 0
        fi
        NOW=$(date +%s)
        [ "$NOW" -lt "$DEADLINE" ] || return 1
        sleep "$SLEEP_SECS"
    done
}

append_source() {
    dst_name="$1"
    src_path="$2"
    [ -n "$src_path" ] || return 0
    [ -f "$src_path" ] || return 0
    printf '%s\t%s\n' "$dst_name" "$src_path" >> "$SOURCE_LIST"
}

append_package_extras() {
    for path in "$PACKAGE_DIR"/*; do
        [ -f "$path" ] || continue
        base="$(basename "$path")"
        case "$base" in
            pjarczak_bambu_linux_host|pjarczak_bambu_linux_host_abi1|pjarczak_bambu_linux_host_abi0|libbambu_networking.so|libBambuSource.so|linux_payload_manifest.json|ca-certificates.crt|slicer_base64.cer)
                continue
                ;;
            *.dll|*.ps1|*.txt|*.tar|*.zip|*.cmd|*.bat|*.sh)
                continue
                ;;
        esac
        append_source "$base" "$path"
    done
}

copy_source_list() {
    while IFS="$(printf '\t')" read -r dst_name src_path; do
        [ -n "$dst_name" ] || continue
        cp "$src_path" "$TMP_DIR/$dst_name"
    done < "$SOURCE_LIST"
}

compute_runtime_hash() {
    {
        while IFS="$(printf '\t')" read -r dst_name src_path; do
            [ -n "$dst_name" ] || continue
            printf '%s\n' "$dst_name"
            sha256sum "$src_path"
        done < "$SOURCE_LIST"
    } | sha256sum | cut -d ' ' -f1
}

list_runtime_files() {
    find -L "$CURRENT_DIR" -maxdepth 1 -type f -printf '%f\n' | sort | tr '\n' ',' | sed 's/,$//'
}

resolve_payload_sources
if [ -z "$NETWORK_SRC" ] || [ -z "$SOURCE_SRC" ]; then
    log "plugin_not_downloaded package_dir=$PACKAGE_DIR plugin_cache_dir=${PLUGIN_CACHE_DIR:-none}"
    if [ "$MODE" = "probe" ]; then
        exit 3
    fi
    if ! wait_for_payload_sources; then
        log "plugin_not_downloaded_timeout package_dir=$PACKAGE_DIR plugin_cache_dir=${PLUGIN_CACHE_DIR:-none}"
        exit 127
    fi
fi

if [ -z "$ABI1_SRC" ] && [ -z "$ABI0_SRC" ]; then
    log "missing host ABI binaries in package_dir=$PACKAGE_DIR plugin_cache_dir=${PLUGIN_CACHE_DIR:-none}"
    exit 127
fi

if ! command -v sha256sum >/dev/null 2>&1; then
    log "sha256sum not found inside WSL distro"
    exit 127
fi

export HOME="${HOME:-/root}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$HOME/.pjarczak-bambu-runtime"
unset APPDATA LOCALAPPDATA USERPROFILE HOMEDRIVE HOMEPATH TEMP TMP TMPDIR

RUNTIME_BASE="${PJARCZAK_BAMBU_WSL_RUNTIME_DIR:-$HOME/.pjarczak-bambu-runtime}"
mkdir -p "$RUNTIME_BASE"
SOURCE_LIST="$(mktemp "$RUNTIME_BASE/.sources.XXXXXX")"
trap 'rm -f "$SOURCE_LIST"' EXIT INT TERM

append_source "pjarczak_bambu_linux_host" "$HOST_SRC"
append_source "libbambu_networking.so" "$NETWORK_SRC"
append_source "libBambuSource.so" "$SOURCE_SRC"
append_source "pjarczak_bambu_linux_host_abi1" "$ABI1_SRC"
append_source "pjarczak_bambu_linux_host_abi0" "$ABI0_SRC"
append_source "linux_payload_manifest.json" "$MANIFEST_SRC"
append_source "ca-certificates.crt" "$CA_BUNDLE_SRC"
append_source "slicer_base64.cer" "$SLICER_CERT_SRC"
append_package_extras

RUNTIME_HASH="$(compute_runtime_hash)"
TARGET_DIR="$RUNTIME_BASE/$RUNTIME_HASH"
CURRENT_DIR="$RUNTIME_BASE/current"

log "mode=$MODE package_dir=$PACKAGE_DIR plugin_cache_dir=${PLUGIN_CACHE_DIR:-none} runtime_hash=$RUNTIME_HASH"
log "wrapper_src=$HOST_SRC"
log "network_src=${NETWORK_SRC:-missing}"
log "source_src=${SOURCE_SRC:-missing}"
log "abi1_src=${ABI1_SRC:-missing}"
log "abi0_src=${ABI0_SRC:-missing}"
log "manifest_src=${MANIFEST_SRC:-missing}"
log "ca_bundle_src=${CA_BUNDLE_SRC:-missing}"
log "slicer_cert_src=${SLICER_CERT_SRC:-missing}"

if [ ! -d "$TARGET_DIR" ]; then
    TMP_DIR="$RUNTIME_BASE/.tmp-$RUNTIME_HASH-$$"
    rm -rf "$TMP_DIR"
    mkdir -p "$TMP_DIR"
    copy_source_list
    chmod 755 "$TMP_DIR/pjarczak_bambu_linux_host" "$TMP_DIR"/pjarczak_bambu_linux_host_abi* 2>/dev/null || true
    mv "$TMP_DIR" "$TARGET_DIR"
    log "created_runtime_dir=$TARGET_DIR"
else
    log "reusing_runtime_dir=$TARGET_DIR"
fi

rm -rf "$CURRENT_DIR"
ln -s "$TARGET_DIR" "$CURRENT_DIR"
export PJARCZAK_BAMBU_PLUGIN_DIR="$CURRENT_DIR"
export PJARCZAK_BAMBU_NETWORK_SO="$CURRENT_DIR/libbambu_networking.so"
export PJARCZAK_BAMBU_SOURCE_SO="$CURRENT_DIR/libBambuSource.so"
export LD_LIBRARY_PATH="$CURRENT_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -f "$CURRENT_DIR/ca-certificates.crt" ]; then
    export SSL_CERT_FILE="$CURRENT_DIR/ca-certificates.crt"
    export CURL_CA_BUNDLE="$CURRENT_DIR/ca-certificates.crt"
fi
if [ -d /etc/ssl/certs ]; then
    export SSL_CERT_DIR="/etc/ssl/certs"
fi

BIN_PATH=$("$CURRENT_DIR/pjarczak_bambu_linux_host" --print-bin 2>/tmp/pjarczak-bin.txt || true)
if [ -z "$BIN_PATH" ] || [ ! -x "$BIN_PATH" ]; then
    log "failed to resolve host binary"
    cat /tmp/pjarczak-bin.txt >&2 || true
    exit 127
fi

log "selected_bin=$BIN_PATH"
log "runtime_files=$(list_runtime_files)"

if command -v ldd >/dev/null 2>&1; then
    LDD_OUT="$(ldd "$BIN_PATH" 2>&1 || true)"
    if printf '%s\n' "$LDD_OUT" | grep -q 'not found'; then
        log "ldd reported missing libraries for $BIN_PATH"
        printf '%s\n' "$LDD_OUT" >&2
        exit 127
    fi
fi

if [ "$MODE" = "probe" ]; then
    echo "probe_ok runtime_dir=$CURRENT_DIR runtime_hash=$RUNTIME_HASH bin=$BIN_PATH"
    exit 0
fi

exec "$CURRENT_DIR/pjarczak_bambu_linux_host"
