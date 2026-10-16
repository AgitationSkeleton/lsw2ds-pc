#!/usr/bin/env python3
"""Verify that the canonical character render directory contains only approved files."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    approval = json.loads((ROOT / "config" / "render_approval.json").read_text(encoding="utf-8"))
    approved = {Path(record["path"]).name for record in approval["approved_character_renders"].values()}
    actual = {path.name for path in (ROOT / "renders" / "characters").glob("*.png")}
    unexpected = sorted(actual - approved)
    missing = sorted(approved - actual)
    if unexpected or missing:
        if unexpected:
            print("unapproved canonical PNGs: " + ", ".join(unexpected))
        if missing:
            print("missing approved PNGs: " + ", ".join(missing))
        return 1
    print(f"Canonical render set valid: {len(approved)} approved character PNG(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
