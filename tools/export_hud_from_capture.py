#!/usr/bin/env python3
"""Turn a captured in-match screen into the port's HUD layout config.

Every number the port needs for the Bounty Hunter touch screen already exists
in the reconstruction manifests - background layer bases and scroll, sprite
positions, sizes, tiles and palettes. Transcribing them into GDScript by hand
is how transcription errors get in, so this derives the config straight from a
capture instead.

Composite sprites are the reason this is not a flat list. The engine builds
non-power-of-two art from several OBJ sprites - the portrait circle is 32x32 +
8x32 + 32x8 forming 40x40, and the customize item icons are 16x16 + 16x8 +
8x16 + 8x8 forming 24x24. Sprites that are adjacent, share a palette and abut
exactly are grouped here into one element, so the port draws a circle rather
than three unrelated rectangles.

Run:
    python tools/export_hud_from_capture.py \\
        --dir out/work/archive/gpudump_session1/inst0 \\
        --output config/bounty_hud_layout.json
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def group_composites(sprites: list[dict]) -> list[dict]:
    """Merge sprites that abut and share a palette into single elements."""
    remaining = sorted(sprites, key=lambda s: (s["palette"], s["y"], s["x"]))
    used: set[int] = set()
    groups = []
    for sprite in remaining:
        if sprite["index"] in used:
            continue
        members = [sprite]
        used.add(sprite["index"])
        changed = True
        while changed:
            changed = False
            left = min(m["x"] for m in members)
            top = min(m["y"] for m in members)
            right = max(m["x"] + m["w"] for m in members)
            bottom = max(m["y"] + m["h"] for m in members)
            for other in remaining:
                if other["index"] in used or other["palette"] != sprite["palette"]:
                    continue
                # touching or overlapping the current bounding box
                if (other["x"] <= right and other["x"] + other["w"] >= left
                        and other["y"] <= bottom and other["y"] + other["h"] >= top):
                    members.append(other)
                    used.add(other["index"])
                    changed = True
        left = min(m["x"] for m in members)
        top = min(m["y"] for m in members)
        groups.append({
            "x": left, "y": top,
            "w": max(m["x"] + m["w"] for m in members) - left,
            "h": max(m["y"] + m["h"] for m in members) - top,
            "palette": sprite["palette"],
            "priority": sprite["priority"],
            "parts": [
                {"x": m["x"] - left, "y": m["y"] - top,
                 "w": m["w"], "h": m["h"], "tile": m["tile"]}
                for m in sorted(members, key=lambda m: (m["y"], m["x"]))
            ],
        })
    return sorted(groups, key=lambda g: (g["y"], g["x"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "config/bounty_hud_layout.json")
    arguments = parser.parse_args()

    manifest = json.loads(
        (arguments.dir / "screen/manifest.json").read_text(encoding="utf-8"))
    touch = manifest["engines"]["sub_touch"]
    top = manifest["engines"]["main_top"]

    groups = group_composites(touch["sprites"])

    # The slot column is the run of *identically sized* composites down the
    # left edge. Selecting merely on "left edge and wide enough" also catches
    # the portraits, which sit three pixels inside each circle at 32x32 - that
    # mixes two interleaved series and yields a nonsense pitch of 3. Group by
    # exact size and take the largest series instead.
    by_size: dict[tuple, list] = {}
    for group in groups:
        if group["x"] < 12:
            by_size.setdefault((group["w"], group["h"]), []).append(group)
    column = max(by_size.values(), key=len) if by_size else []
    column.sort(key=lambda g: g["y"])
    pitch = column[1]["y"] - column[0]["y"] if len(column) >= 2 else 0

    # Portraits are the smaller series, drawn inset within each slot.
    portraits = []
    for size, series in by_size.items():
        if series is not column and len(series) >= 1:
            portraits.extend(series)
    portraits.sort(key=lambda g: g["y"])
    inset = None
    if column and portraits:
        inset = [portraits[0]["x"] - column[0]["x"],
                 portraits[0]["y"] - column[0]["y"]]

    document = {
        "note": (
            "Bounty Hunter touch screen, derived from a captured in-match frame "
            "by tools/export_hud_from_capture.py. Coordinates are DS pixels on "
            "a 256x192 screen. Sprites that abut and share a palette are "
            "grouped as composites, because the engine builds non-power-of-two "
            "art from several OBJ sprites - the portrait circle is 32x32 + "
            "8x32 + 32x8 forming 40x40."
        ),
        "source_capture": str(arguments.dir).replace("\\", "/"),
        "screen": [256, 192],
        "touch": {
            "bg_mode": touch["bg_mode"],
            "layers_enabled": touch["layers_enabled"],
            "backgrounds": [
                {k: b[k] for k in ("index", "priority", "char_base",
                                   "screen_base", "colour256", "xpos", "ypos",
                                   "opaque_px")}
                for b in touch["backgrounds"]
            ],
            "elements": groups,
        },
        "top": {
            "layers_enabled": top["layers_enabled"],
            "backgrounds": [
                {k: b[k] for k in ("index", "priority", "char_base",
                                   "screen_base", "colour256", "opaque_px")}
                for b in top["backgrounds"]
            ],
        },
        "slot_column": {
            "count": len(column),
            "origin": [column[0]["x"], column[0]["y"]] if column else None,
            "pitch": pitch,
            "size": [column[0]["w"], column[0]["h"]] if column else None,
            "palettes": [g["palette"] for g in column],
            "portrait_inset": inset,
            "portrait_palettes": [g["palette"] for g in portraits],
            "note": ("The column is a live scoreboard sorted by score, so the row "
                     "is a ranking and the palette is the player's identity. "
                     "Portraits are a second, smaller series drawn inset inside "
                     "each circle - only occupied slots have one."),
        },
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=2) + "\n",
                                encoding="utf-8")

    print(f"{len(touch['sprites'])} sprites -> {len(groups)} composite elements")
    for g in groups:
        parts = "+".join(f"{p['w']}x{p['h']}" for p in g["parts"])
        print(f"  ({g['x']:3d},{g['y']:3d}) {g['w']:2d}x{g['h']:<2d} pal{g['palette']:2d}  {parts}")
    print(f"\nslot column: {len(column)} slots, pitch {pitch}, "
          f"palettes {[g['palette'] for g in column]}")
    print(f"-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
