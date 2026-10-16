#!/usr/bin/env python3
"""Search all extracted catalog names, optionally filtering archive extensions."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("terms", nargs="+")
    parser.add_argument("--extension", action="append", dest="extensions")
    parser.add_argument(
        "--catalog",
        type=Path,
        default=Path("out/raw/archive/catalog.json"),
    )
    args = parser.parse_args()
    terms = tuple(term.casefold() for term in args.terms)
    allowed = set(args.extensions or [])
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    for entry in catalog["entries"]:
        if allowed and entry["extension"] not in allowed:
            continue
        names = entry.get("names", [])
        if not any(term in " ".join(names).casefold() for term in terms):
            continue
        print(
            f"{entry['asset_id']:04d} {entry['extension']:<6} "
            f"{entry['expanded_size']:8d} | {' | '.join(names[:20])}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
