#!/usr/bin/env python3
"""Inventory likely character-related entries from an extracted LSW2 DS archive."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


DEFAULT_TERMS = (
    "minikit",
    "greedo",
    "character",
    "hero",
    "player",
    "bounty",
    "han",
    "luke",
    "leia",
    "vader",
    "storm",
    "jedi",
    "sith",
    "droid",
    "wookie",
    "chewie",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "catalog",
        nargs="?",
        type=Path,
        default=Path("out/raw/archive/catalog.json"),
    )
    parser.add_argument("--term", action="append", dest="terms")
    parser.add_argument("--all-models", action="store_true")
    args = parser.parse_args()

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    terms = tuple(term.lower() for term in (args.terms or DEFAULT_TERMS))
    for entry in catalog["entries"]:
        if entry["extension"] not in {"nsbmd", "bin", "btx0"}:
            continue
        names = entry.get("names", [])
        haystack = " ".join(names).lower()
        if not (args.all_models and entry["extension"] == "nsbmd") and not any(
            term in haystack for term in terms
        ):
            continue
        print(
            f"{entry['asset_id']:04d} {entry['extension']:<5} "
            f"{entry['expanded_size']:8d} | {' | '.join(names[:12])}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
