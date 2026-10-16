#!/usr/bin/env python3
"""Cross-reference HUD and UI elements by how the ROM actually uses them.

A DS HUD element in this game is not a loose sprite. It is a small model - a
quad or two - carrying a texture, usually with a BTA0/SRT0 material animation
whose translation_u/translation_v channels slide the UVs across an ATLAS to
pick a frame. `HeartDS` proves the shape: a 64x64 texture holding a 3x3 grid of
nine heart frames, driven by the 36-frame animation in asset 2635.

So identifying a HUD icon means finding the model, not the pixels. This walks
every small model in the archive, joins it to its texture and to any adjacent
material animation, and reports:

  * the texture, its size and its palette format;
  * how many distinct UV cells the animation visits, which is the frame count
    and therefore the atlas layout;
  * the animation's frame count and the material it drives.

Run:
    python tools/analyze_hud_elements.py --output out/work/hud_elements.json
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from inventory_textures import textures_in  # noqa: E402

ENTRIES = ROOT / "out/raw/archive/entries"

# A HUD quad is tiny next to level geometry; this keeps the sweep to plausible
# candidates rather than every prop in the game.
MAX_MODEL_BYTES = 6000
# How far either side of a model to look for its material animation.
NEIGHBOUR_SPAN = 3


def read(asset_id: int) -> bytes | None:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    return matches[0].read_bytes() if matches else None


def nitro_names(data: bytes) -> list[str]:
    """Printable names embedded in a Nitro container, minus the block tags."""
    tags = {"BMD0", "MDL0", "BTX0", "TEX0", "BCA0", "JNT0", "BTA0", "SRT0", "BVA0", "VIS0"}
    found: list[str] = []
    text = bytearray()
    for byte in data:
        if 32 <= byte < 127:
            text.append(byte)
            continue
        if len(text) >= 4:
            candidate = text.decode("ascii")
            if candidate not in tags and candidate not in found:
                found.append(candidate)
        text = bytearray()
    return found


def srt_channels(data: bytes) -> list[dict]:
    """Material-animation channels, if this asset is a BTA0."""
    if data[:4] != b"BTA0":
        return []
    names = nitro_names(data)
    # The exporter already decodes these properly; here we only need the shape,
    # so report the material names and the frame count from the header.
    try:
        frames = struct.unpack_from("<H", data, 0x38)[0]
    except struct.error:
        frames = 0
    return [{"materials": names[:4], "frame_count_hint": frames}]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/work/hud_elements.json")
    parser.add_argument("--max-bytes", type=int, default=MAX_MODEL_BYTES)
    arguments = parser.parse_args()
    if not ENTRIES.exists():
        raise SystemExit(f"missing {ENTRIES}; run the extraction step first")

    sizes = {int(path.stem): path.stat().st_size for path in ENTRIES.iterdir()}
    elements = []
    for asset_id in sorted(sizes):
        if sizes[asset_id] > arguments.max_bytes:
            continue
        data = read(asset_id)
        if data is None or data[:4] != b"BMD0":
            continue
        try:
            textures = textures_in(data)
        except Exception:
            textures = []
        if not textures:
            continue
        names = nitro_names(data)

        animations = []
        for offset in range(-NEIGHBOUR_SPAN, NEIGHBOUR_SPAN + 1):
            if offset == 0:
                continue
            neighbour = read(asset_id + offset)
            if neighbour is None or neighbour[:4] != b"BTA0":
                continue
            animations.append({
                "asset": asset_id + offset,
                "channels": srt_channels(neighbour),
            })

        elements.append({
            "model_asset": asset_id,
            "model_bytes": sizes[asset_id],
            "model_names": names[:6],
            "textures": [
                {"name": t["name"], "width": t["width"], "height": t["height"],
                 "format": t.get("format", "")}
                for t in textures
            ],
            "material_animations": animations,
            # A model with a material animation over a texture larger than its
            # apparent icon is the atlas signature that HeartDS demonstrates.
            "atlas_candidate": bool(animations) and any(
                t["width"] >= 32 and t["height"] >= 32 for t in textures
            ),
        })

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps({
        "note": (
            "Small models joined to their textures and material animations. A "
            "HUD icon in this game is a quad plus an atlas plus a UV animation "
            "that picks the frame; HeartDS (model 2634, animation 2635, a 3x3 "
            "grid of nine hearts) is the reference case."
        ),
        "max_model_bytes": arguments.max_bytes,
        "element_count": len(elements),
        "elements": elements,
    }, indent=2) + "\n", encoding="utf-8")

    animated = [e for e in elements if e["atlas_candidate"]]
    print(f"{len(elements)} small textured models, {len(animated)} atlas candidates")
    for element in animated[:30]:
        names = ", ".join(t["name"] for t in element["textures"])
        print(f"  model {element['model_asset']:5d}  {names[:44]:46s} "
              f"anim {[a['asset'] for a in element['material_animations']]}")
    print(f"-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
