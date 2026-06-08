#!/usr/bin/env python3
"""Download all SD backup files exposed by the M5 HTTP backup server."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.parse
import urllib.request
from pathlib import Path


def fetch_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=15) as response:
        return json.loads(response.read().decode("utf-8"))


def download_file(base_url: str, remote_path: str, output_root: Path) -> None:
    query = urllib.parse.urlencode({"path": remote_path})
    url = urllib.parse.urljoin(base_url.rstrip("/") + "/", "download") + f"?{query}"
    relative_path = remote_path.lstrip("/")
    target_path = output_root / relative_path
    target_path.parent.mkdir(parents=True, exist_ok=True)

    with urllib.request.urlopen(url, timeout=30) as response:
        target_path.write_bytes(response.read())

    print(f"{remote_path} -> {target_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Download ToughTools SD backup files over WLAN.")
    parser.add_argument("base_url", help="Device URL, for example http://192.168.1.42/")
    parser.add_argument("output_dir", help="Local directory for downloaded files")
    args = parser.parse_args()

    base_url = args.base_url.rstrip("/") + "/"
    output_root = Path(args.output_dir)
    output_root.mkdir(parents=True, exist_ok=True)

    manifest_url = urllib.parse.urljoin(base_url, "manifest.json")
    manifest = fetch_json(manifest_url)
    files = manifest.get("files", [])
    if not isinstance(files, list):
        print("Manifest has no files list", file=sys.stderr)
        return 1

    for item in files:
        remote_path = item.get("path") if isinstance(item, dict) else None
        if not isinstance(remote_path, str) or not remote_path.startswith("/"):
            print(f"Skipping invalid manifest entry: {item!r}", file=sys.stderr)
            continue
        download_file(base_url, remote_path, output_root)

    print(f"Downloaded {len(files)} file(s) to {output_root.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
