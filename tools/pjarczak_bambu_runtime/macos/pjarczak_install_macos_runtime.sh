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

ensure_qemu_installed() {
    # Lima with --vm-type=qemu --arch=x86_64 (used on Apple Silicon to run
    # the guest as a native x86-64 Linux VM) requires qemu-system-x86_64.
    # Lima itself does not bundle qemu — it expects a host install.
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        return 0
    fi
    if command -v brew >/dev/null 2>&1; then
        brew install qemu
        return $?
    fi
    echo "qemu-system-x86_64 not found and Homebrew is unavailable; install QEMU manually before retrying" >&2
    return 1
}

ensure_lima_x86_64_guestagent() {
    # When running a non-native-arch guest, Lima needs the matching guestagent
    # binary. Homebrew's `lima` formula only ships the host-arch guestagent;
    # the foreign-arch ones live in `lima-additional-guestagents`. The locally
    # bundled tarball path (install_lima_binary_locally) does fetch them, but
    # when Lima was installed via Homebrew we have to ensure them separately.
    local share_dirs=()
    if [[ -d "$LOCAL_LIMA_ROOT/share/lima" ]]; then
        share_dirs+=("$LOCAL_LIMA_ROOT/share/lima")
    fi
    if command -v brew >/dev/null 2>&1; then
        local brew_prefix
        brew_prefix=$(brew --prefix lima 2>/dev/null || true)
        if [[ -n "$brew_prefix" && -d "$brew_prefix/share/lima" ]]; then
            share_dirs+=("$brew_prefix/share/lima")
        fi
    fi
    local d
    for d in "${share_dirs[@]}"; do
        if [[ -e "$d/lima-guestagent.Linux-x86_64" || -e "$d/lima-guestagent.Linux-x86_64.gz" ]]; then
            return 0
        fi
    done
    if command -v brew >/dev/null 2>&1; then
        if brew install lima-additional-guestagents; then
            return 0
        fi
    fi
    echo "lima-guestagent.Linux-x86_64 is missing and could not be installed; foreign-arch Lima guests will fail to start" >&2
    return 1
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
ensure_qemu_installed
ensure_lima_x86_64_guestagent

# We always run the guest as a native x86-64 Linux VM, including on Apple
# Silicon. We deliberately do NOT use VZ + Rosetta: the Bambu plugin .so
# binaries make memory-layout assumptions (canonical 47-bit user VA, pointer
# bit-tagging in upper bits) that hold under a native x86-64 kernel but break
# under Rosetta translation — manifesting as SIGBUS / std::bad_alloc / stack
# canary failures. Full-system QEMU emulation gives the binary the canonical
# x86-64 environment it expects, at the cost of ~10x slower CPU.
START_ARGS=(start "--name=${INSTANCE}" --tty=false --mount-writable --vm-type=qemu --arch=x86_64)

# OrcaSlicer writes the .3mf for cloud/LAN print upload to /var/folders/...
# (the macOS per-user temp tree under TMPDIR). The bridge in the guest reads
# the file by absolute path, so /var/folders must be visible inside the VM
# in addition to the default $HOME mount. Without it, "Send to Printer"
# hangs at "sending through cloud service" forever.
START_ARGS+=(--mount=/var/folders:w)

# If a previous install created the instance under a different arch (e.g.
# the older vz+rosetta arm64 setup), recreate it. Lima cannot change arch in
# place.
existing_arch=""
if "$LIMACTL" list --format '{{.Arch}}' "$INSTANCE" >/dev/null 2>&1; then
    existing_arch=$("$LIMACTL" list --format '{{.Arch}}' "$INSTANCE" 2>/dev/null || true)
fi
if [[ -n "$existing_arch" && "$existing_arch" != "x86_64" ]]; then
    echo "existing Lima instance arch '$existing_arch' does not match required 'x86_64'; recreating" >&2
    REPLACE_EXISTING=1
fi

if [[ "$REPLACE_EXISTING" -eq 1 ]]; then
    "$LIMACTL" stop "$INSTANCE" >/dev/null 2>&1 || true
    "$LIMACTL" delete "$INSTANCE" >/dev/null 2>&1 || true
fi

# Three states to handle:
#  1. Reachable      -> nothing to do.
#  2. Exists/stopped -> start the existing instance (NOT create).
#  3. Doesn't exist  -> create+start from the default template.
# The previous version skipped case 2 and always fell into case 3, which
# fails with "instance already exists" whenever the VM is just stopped.
if "$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1; then
    :
elif "$LIMACTL" list --format '{{.Name}}' 2>/dev/null | grep -Fxq "$INSTANCE"; then
    "$LIMACTL" start "$INSTANCE"
else
    "$LIMACTL" "${START_ARGS[@]}" template:default
fi

"$LIMACTL" start-at-login "$INSTANCE" --enabled >/dev/null 2>&1 || true
"$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null

# Provision the x86-64 (amd64) userspace inside the guest so the prebuilt Linux
# host binaries can run under Rosetta. The binaries are x86-64 ELFs; Rosetta
# translates the CPU instructions, but the Linux loader still requires the
# x86-64 dynamic linker and matching shared libraries to exist on disk. On a
# fresh aarch64 Ubuntu cloud image these are not present, and apt cannot fetch
# them by default because the cloud-init sources.list points only at
# ports.ubuntu.com (which doesn't host amd64). We:
#   1. pin the existing arm64 sources to Architectures: arm64 so apt stops
#      asking ports.ubuntu.com for amd64 (which 404s),
#   2. add an amd64-only source pointing at archive.ubuntu.com,
#   3. dpkg --add-architecture amd64 and install the libs that
#      libbambu_networking.so / libBambuSource.so need.
provision_guest_amd64_userspace() {
    if [[ "$(uname -m)" != "arm64" ]]; then
        return 0
    fi
    # If the guest is itself x86_64 (the default since we switched to a full
    # QEMU x86_64 VM), all amd64 libs are already native — no multiarch setup
    # needed. Bail out before touching apt.
    local guest_arch
    guest_arch=$("$LIMACTL" shell "$INSTANCE" -- uname -m 2>/dev/null || true)
    if [[ "$guest_arch" == "x86_64" ]]; then
        return 0
    fi
    "$LIMACTL" shell "$INSTANCE" -- sudo bash -s <<'GUEST_PROVISION'
set -e

# Skip if amd64 userspace is already provisioned. The x86-64 dynamic linker is
# the canonical marker.
if [[ -e /lib64/ld-linux-x86-64.so.2 ]] && \
   dpkg -l libc6:amd64 >/dev/null 2>&1; then
    exit 0
fi

DEB822=/etc/apt/sources.list.d/ubuntu.sources
if [[ -f "$DEB822" ]]; then
    if ! grep -q '^Architectures:' "$DEB822" 2>/dev/null; then
        cp "$DEB822" "${DEB822}.bak"
        python3 - "$DEB822" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
stanzas = re.split(r"(\n\s*\n)", text)
out = []
for s in stanzas:
    if re.search(r"^\s*Types:", s, re.MULTILINE) and \
       not re.search(r"^\s*Architectures:", s, re.MULTILINE):
        s = s.rstrip("\n") + "\nArchitectures: arm64\n"
    out.append(s)
open(path, "w").write("".join(out))
PY
    fi
fi

CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME")
cat > /etc/apt/sources.list.d/ubuntu-amd64.sources <<EOF
Types: deb
URIs: http://archive.ubuntu.com/ubuntu
Suites: ${CODENAME} ${CODENAME}-updates ${CODENAME}-backports
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://security.ubuntu.com/ubuntu
Suites: ${CODENAME}-security
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

dpkg --add-architecture amd64
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libc6:amd64 \
    libstdc++6:amd64 \
    libgcc-s1:amd64 \
    zlib1g:amd64 \
    libssl3:amd64
GUEST_PROVISION
}

provision_guest_amd64_userspace

printf 'runtime installed
'
