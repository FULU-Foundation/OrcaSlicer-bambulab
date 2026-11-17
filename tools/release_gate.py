#!/usr/bin/env python3
"""release_gate.py — OrcaSlicer-bambulab release gate. GATE VERSION 1.0.

ORIGIN (owned transfer): this gate adapts the fail-closed, hash-chained
gate concept built for the VimStick compilation stitch gate
(~/workspace/vimstick/production/compilation/stitch_gate.py, gate v2.1,
2026-11-08). The stitch gate exists because a finished artifact shipped
defects that no spot check caught; its law is that a PASS must be bound
to the exact bytes that were checked, and that a check which errors or
does not run is a FAIL, never a pass. This gate applies the same law to
software releases.

WHY A RELEASE NEEDS THIS: a slicer release is a set of binaries that
users log into a printer vendor's cloud with. If an artifact is missing,
empty (truncated upload), or silently replaced after the checksums were
published, users cannot tell. This gate makes that detectable:

  1. SCAN     — the release directory must exist and contain at least
                one artifact. A missing or empty directory is a FAIL.
  2. INTEGRITY — every artifact must be non-empty. A zero-byte artifact
                is a FAIL (truncated or placeholder upload).
  3. EXPECTED — when --expect names are given, every named artifact
                must be present. A missing expected artifact is a FAIL.
  4. SEAL     — SHA-256 is computed for every artifact and written to
                SHA256SUMS inside the release directory (seal mode).
  5. VERIFY   — in --verify mode, every artifact is re-hashed and
                compared against SHA256SUMS. Any mismatch, any file in
                SHA256SUMS that is now missing, and any artifact present
                but absent from SHA256SUMS is a FAIL.

FAIL-CLOSED DESIGN: there is no override flag. A check that errors is a
FAIL, a check that did not run is a FAIL, and a PASS is written only
when every check actually ran and passed. Every run, pass or fail, is
appended to a hash-chained ledger (gate-ledger.jsonl) together with the
SHA-256 of every artifact, so a PASS is bound to the exact bytes that
were gated: change one byte of one artifact and the PASS no longer
describes it (verify mode will FAIL, and the ledger shows both runs).

Usage:
    release_gate.py RELEASE_DIR [--expect NAME ...]
                    [--ledger PATH] [--sums-name SHA256SUMS]
    release_gate.py RELEASE_DIR --verify [--ledger PATH]

Exit 0 = PASS (release is sealed / verified), 1 = FAIL (do not publish).
"""

import argparse
import hashlib
import json
import os
import sys
import time

GATE_VERSION = "1.0"
SUMS_DEFAULT = "SHA256SUMS"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ledger_append(ledger_path, entry):
    """Append one entry, hash-chained to the previous ledger line.

    Same construction as stitch_gate.py: prev is the SHA-256 of the
    last raw ledger line, or the literal GENESIS for the first entry.
    Returns the 1-based entry number.
    """
    prev = "GENESIS"
    n = 0
    if os.path.isfile(ledger_path):
        with open(ledger_path, "rb") as fh:
            lines = [ln for ln in fh.read().splitlines() if ln.strip()]
        if lines:
            prev = hashlib.sha256(lines[-1]).hexdigest()
            n = len(lines)
    entry["prev"] = prev
    parent = os.path.dirname(os.path.abspath(ledger_path))
    os.makedirs(parent, exist_ok=True)
    with open(ledger_path, "a") as fh:
        fh.write(json.dumps(entry, sort_keys=True) + "\n")
    return n + 1


def list_artifacts(release_dir, sums_name):
    """Regular files directly inside release_dir, excluding SHA256SUMS."""
    names = []
    for name in sorted(os.listdir(release_dir)):
        if name == sums_name:
            continue
        full = os.path.join(release_dir, name)
        if os.path.isfile(full):
            names.append(name)
    return names


def run_gate(release_dir, expect, verify, sums_name):
    """Run all checks. Returns (verdict, checks, artifacts, detail)."""
    checks = {}
    artifacts = {}
    details = []

    def fail(name, detail):
        checks[name] = "FAIL"
        details.append(f"{name}: {detail}")

    # 1. SCAN — fail closed if the directory itself is unusable.
    try:
        if not os.path.isdir(release_dir):
            fail("SCAN", f"release directory does not exist: {release_dir}")
            return "FAIL", checks, artifacts, "; ".join(details)
        names = list_artifacts(release_dir, sums_name)
        if not names:
            fail("SCAN", "release directory contains no artifacts")
            return "FAIL", checks, artifacts, "; ".join(details)
        checks["SCAN"] = "PASS"
    except Exception as e:  # fail-closed: an errored check is a FAIL
        fail("SCAN", f"check errored: {type(e).__name__}: {e}")
        return "FAIL", checks, artifacts, "; ".join(details)

    # 2. INTEGRITY — no zero-byte artifacts; hash everything.
    try:
        empty = []
        for name in names:
            full = os.path.join(release_dir, name)
            if os.path.getsize(full) == 0:
                empty.append(name)
            artifacts[name] = sha256_of(full)
        if empty:
            fail("INTEGRITY", f"empty artifact(s): {', '.join(empty)}")
        else:
            checks["INTEGRITY"] = "PASS"
    except Exception as e:
        fail("INTEGRITY", f"check errored: {type(e).__name__}: {e}")

    # 3. EXPECTED — only when the caller names expected artifacts.
    if expect:
        try:
            missing = [n for n in expect if n not in artifacts]
            if missing:
                fail("EXPECTED", f"missing expected artifact(s): {', '.join(missing)}")
            else:
                checks["EXPECTED"] = "PASS"
        except Exception as e:
            fail("EXPECTED", f"check errored: {type(e).__name__}: {e}")

    sums_path = os.path.join(release_dir, sums_name)
    if verify:
        # 4/5 VERIFY — recompute against the sealed SHA256SUMS.
        try:
            if not os.path.isfile(sums_path):
                fail("VERIFY", f"no {sums_name} in release directory — release was never sealed")
            else:
                sealed = {}
                with open(sums_path) as fh:
                    for line in fh:
                        line = line.rstrip("\n")
                        if not line.strip():
                            continue
                        digest, _, name = line.partition("  ")
                        if not name:
                            digest, _, name = line.partition(" ")
                        sealed[name.strip()] = digest.strip()
                problems = []
                for name, digest in sealed.items():
                    if name not in artifacts:
                        problems.append(f"{name}: in {sums_name} but missing from directory")
                    elif artifacts[name] != digest:
                        problems.append(f"{name}: checksum mismatch")
                for name in artifacts:
                    if name not in sealed:
                        problems.append(f"{name}: present but not in {sums_name}")
                if problems:
                    fail("VERIFY", "; ".join(problems))
                else:
                    checks["VERIFY"] = "PASS"
        except Exception as e:
            fail("VERIFY", f"check errored: {type(e).__name__}: {e}")
    else:
        # 4 SEAL — write SHA256SUMS bound to the exact bytes hashed above.
        # Sealing only makes sense if the earlier checks passed.
        try:
            if checks.get("INTEGRITY") != "PASS" or (
                expect and checks.get("EXPECTED") != "PASS"
            ):
                fail("SEAL", "not sealed — an earlier check failed")
            else:
                with open(sums_path, "w") as fh:
                    for name in sorted(artifacts):
                        fh.write(f"{artifacts[name]}  {name}\n")
                checks["SEAL"] = "PASS"
        except Exception as e:
            fail("SEAL", f"check errored: {type(e).__name__}: {e}")

    verdict = "PASS" if all(v == "PASS" for v in checks.values()) else "FAIL"
    return verdict, checks, artifacts, "; ".join(details)


def main():
    ap = argparse.ArgumentParser(description="Fail-closed, hash-chained release gate.")
    ap.add_argument("release_dir", help="directory containing the release artifacts")
    ap.add_argument("--expect", action="append", default=[], metavar="NAME",
                    help="artifact that must be present (repeatable)")
    ap.add_argument("--verify", action="store_true",
                    help="verify artifacts against the sealed SHA256SUMS instead of sealing")
    ap.add_argument("--sums-name", default=SUMS_DEFAULT)
    ap.add_argument("--ledger", default=None,
                    help="ledger path (default: gate-ledger.jsonl beside this script)")
    a = ap.parse_args()

    ledger_path = a.ledger or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "gate-ledger.jsonl")

    try:
        verdict, checks, artifacts, detail = run_gate(
            a.release_dir, a.expect, a.verify, a.sums_name)
    except Exception as e:  # fail-closed at the top level too
        verdict, checks, artifacts = "FAIL", {"GATE": "FAIL"}, {}
        detail = f"gate errored: {type(e).__name__}: {e}"

    entry = {
        "v": GATE_VERSION,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "mode": "verify" if a.verify else "seal",
        "release_dir": os.path.abspath(a.release_dir),
        "artifacts": artifacts,
        "checks": checks,
        "verdict": verdict,
    }
    lineno = ledger_append(ledger_path, entry)

    mode = "VERIFY" if a.verify else "SEAL"
    print(f"RELEASE GATE v{GATE_VERSION} — {mode} — {os.path.abspath(a.release_dir)}")
    for name, status in checks.items():
        print(f"  {name}: {status}")
    for name in sorted(artifacts):
        print(f"  sha256 {artifacts[name][:16]}…  {name}")
    if detail:
        print(f"  detail: {detail}")
    print(f"VERDICT: {verdict} — ledger entry #{lineno} (hash-chained) in {ledger_path}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
