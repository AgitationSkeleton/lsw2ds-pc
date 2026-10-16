#!/usr/bin/env python3
"""Correct the 5-bit to 8-bit expansion in every already-extracted asset.

Every extractor here expanded a 5-bit DS colour channel as `v * 255 // 31`.
That is not what the hardware does, and it is not what melonDS does when it
builds the framebuffer these assets are checked against. From
GPU2D_Soft.cpp:296-305 the internal value is 6-bit (the 5-bit palette entry
doubled), it is shifted into the top 6 bits of the byte, and the top 2 bits are
replicated into the bottom 2:

    out8 = (v6 << 2) | (v6 >> 4),  v6 = v5 << 1
         = (v5 << 3) | (v5 >> 3)

The two agree on most values but not all, and the disagreements are visible in a
direct comparison against a DS frame:

    5-bit    wrong    correct
        7       57         56
        9       74         73
       31      255        251

So nothing extracted so far was ever pure white - the DS's white is 251.

Rather than re-run every extractor, this remaps in place: each of the 32 wrong
values maps to exactly one correct value, and every channel in a correctly
extracted asset must already be one of those 32. Any file containing a value
outside the set is reported and left alone, because that means it did not come
from this pipeline (a screenshot crop, say) and remapping it would be wrong.

    python tools/fix_asset_palettes.py --apply
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

WRONG = {v: v * 255 // 31 for v in range(32)}
RIGHT = {v: (v << 3) | (v >> 3) for v in range(32)}


def build_table() -> tuple[np.ndarray, set[int]]:
    """256-entry LUT; identity for anything not produced by the old formula."""
    table = np.arange(256, dtype=np.uint8)
    valid = set()
    for v in range(32):
        table[WRONG[v]] = RIGHT[v]
        valid.add(WRONG[v])
    return table, valid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--roots", nargs="*", type=Path,
                        default=[ROOT / "godot/assets/ui_rom"])
    parser.add_argument("--apply", action="store_true",
                        help="without this, only reports what would change")
    arguments = parser.parse_args()

    table, valid = build_table()
    changed = skipped = clean = 0

    for root in arguments.roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.png")):
            image = Image.open(path).convert("RGBA")
            data = np.array(image)
            rgb = data[..., :3]
            alpha = data[..., 3]

            # Only opaque pixels carry meaningful colour; a fully transparent
            # pixel's RGB is whatever the writer left behind.
            present = set(np.unique(rgb[alpha > 0]).tolist())
            # Already corrected files hold the RIGHT values, which are mostly
            # not in the wrong set - without this they would be reported as
            # foreign every time the tool is re-run.
            if present and present <= set(RIGHT.values()):
                clean += 1
                continue
            stray = present - valid
            if stray:
                print("  SKIP %-34s %d channel value(s) outside the palette set, "
                      "e.g. %s - not from this pipeline"
                      % (path.name, len(stray), sorted(stray)[:4]))
                skipped += 1
                continue

            fixed = table[rgb]
            if np.array_equal(fixed, rgb):
                clean += 1
                continue

            moved = int((fixed != rgb).any(axis=2)[alpha > 0].sum())
            print("  %-38s %d opaque pixel(s) recoloured"
                  % (path.as_posix(), moved))
            changed += 1
            if arguments.apply:
                data[..., :3] = fixed
                Image.fromarray(data).save(path)

    print("\n%d file(s) %s, %d already correct, %d skipped"
          % (changed, "corrected" if arguments.apply else "would change",
             clean, skipped))
    if not arguments.apply and changed:
        print("Re-run with --apply to write them.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
