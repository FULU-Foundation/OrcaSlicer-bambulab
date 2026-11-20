#!/usr/bin/env python3
import argparse, hashlib, json, re
from pathlib import Path

FALLBACK_ABI_VERSION = '02.05.02.58'

def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def default_abi_version() -> str:
    header = Path(__file__).resolve().parents[1] / 'src' / 'slic3r' / 'Utils' / 'bambu_networking.hpp'
    try:
        text = header.read_text(encoding='utf-8', errors='ignore')
    except OSError:
        return FALLBACK_ABI_VERSION
    m = re.search(r'BAMBU_NETWORK_AGENT_VERSION\s+"([^"]+)"', text)
    return m.group(1) if m else FALLBACK_ABI_VERSION

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('plugin_dir', type=Path)
    ap.add_argument('--abi-version', default=None)
    ap.add_argument('--out', type=Path, default=None)
    args = ap.parse_args()
    abi_version = args.abi_version if args.abi_version is not None else default_abi_version()
    net = args.plugin_dir / 'libbambu_networking.so'
    src = args.plugin_dir / 'libBambuSource.so'
    if not net.exists() or not src.exists():
        raise SystemExit('missing linux payload files')
    files = [
        {'name': net.name, 'sha256': sha256(net), 'abi_version': abi_version},
        {'name': src.name, 'sha256': sha256(src)},
    ]
    for extra_name in ('liblive555.so', 'libagora_rtc_sdk.so', 'libagora-fdkaac.so'):
        extra = args.plugin_dir / extra_name
        if extra.exists():
            files.append({'name': extra.name, 'sha256': sha256(extra)})
    manifest = {'files': files}
    out = args.out or (args.plugin_dir / 'linux_payload_manifest.json')
    out.write_text(json.dumps(manifest, indent=2) + '\n')
    print(out)

if __name__ == '__main__':
    main()
