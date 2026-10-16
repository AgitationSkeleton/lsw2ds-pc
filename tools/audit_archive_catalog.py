#!/usr/bin/env python3
"""Check an extracted archive catalog for duplicate or missing asset IDs."""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "catalog",
        nargs="?",
        type=Path,
        default=Path("out/raw/archive/catalog.json"),
    )
    parser.add_argument("--expected", type=int, default=5548)
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    counts = collections.Counter(entry["asset_id"] for entry in catalog["entries"])
    missing = [asset_id for asset_id in range(args.expected) if counts[asset_id] == 0]
    duplicates = {asset_id: count for asset_id, count in counts.items() if count > 1}
    print(f"records={len(catalog['entries'])} unique={len(counts)} expected={args.expected}")
    print(f"missing ({len(missing)}): {missing}")
    print(f"duplicates ({len(duplicates)}): {duplicates}")
    return 1 if missing or duplicates else 0


if __name__ == "__main__":
    raise SystemExit(main())
