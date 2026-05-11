#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import urllib.request

URLS = [
    "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp",
    "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp",
]


def default_dest() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2] / "src" / "nlohmann" / "json.hpp"


def valid_header(text: str) -> bool:
    return "NLOHMANN_JSON_VERSION_MAJOR" in text and "namespace nlohmann" in text


def download(url: str) -> str:
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read()
    return data.decode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dest", default=str(default_dest()))
    args = parser.parse_args()

    dest = pathlib.Path(args.dest).resolve()
    if dest.is_file() and dest.stat().st_size > 0:
        text = dest.read_text(encoding="utf-8", errors="ignore")
        if valid_header(text):
            print(f"nlohmann json already present: {dest}")
            return 0

    dest.parent.mkdir(parents=True, exist_ok=True)

    last_error = None
    for url in URLS:
        try:
            print(f"downloading {url}")
            text = download(url)
            if not valid_header(text):
                raise RuntimeError("downloaded file does not look like nlohmann/json.hpp")
            with tempfile.NamedTemporaryFile("w", delete=False, encoding="utf-8", dir=str(dest.parent)) as tmp:
                tmp.write(text)
                tmp_path = pathlib.Path(tmp.name)
            tmp_path.replace(dest)
            print(f"saved {dest}")
            return 0
        except Exception as exc:
            last_error = exc
            print(f"failed {url}: {exc}", file=sys.stderr)

    print(f"failed to fetch nlohmann/json.hpp: {last_error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
