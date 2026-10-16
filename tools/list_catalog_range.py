#!/usr/bin/env python3
"""Print an annotated inclusive asset-ID range from the extracted catalog."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=int)
    parser.add_argument("last", type=int)
    parser.add_argument(
        "--catalog",
        type=Path,
        default=Path("out/raw/archive/catalog.json"),
    )
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    by_id = {entry["asset_id"]: entry for entry in catalog["entries"]}
    for asset_id in range(args.first, args.last + 1):
        entry = by_id[asset_id]
        print(
            f"{asset_id:04d} {entry['extension']:<6} "
            f"{entry['expanded_size']:8d} wrapper={str(entry['wrapper']):<4} "
            f"magic={entry['magic']:<8} | {' | '.join(entry.get('names', [])[:16])}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
