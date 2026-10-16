#!/usr/bin/env python3
"""Decode every standard Human_Model actor appearance companion to PNG."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from PIL import Image


TABLE_ADDRESS = 0x020F3E68
ARM9_BASE = 0x02000000
FIRST_ACTOR_ID = 45
RECORD_SIZE = 44
RECORD_COUNT = 62
TTGA = 0x41475454


def rgb555(value: int) -> tuple[int, int, int, int]:
    channels = (value & 0x1F, (value >> 5) & 0x1F, (value >> 10) & 0x1F)
    return tuple((channel << 3) | (channel >> 2) for channel in channels) + (255,)


def decode(payload: bytes, default_palette_bytes: int = 512) -> tuple[Image.Image, dict]:
    header_bytes = 0
    if len(payload) >= 8 and struct.unpack_from("<I", payload)[0] == TTGA:
        palette_bytes = struct.unpack_from("<I", payload, 4)[0]
        payload = payload[8:]
        header_bytes = 8
    else:
        palette_bytes = default_palette_bytes
    texture = payload[palette_bytes:]
    if palette_bytes % 2 or len(texture) != 1024:
        raise ValueError(f"unsupported split palette={palette_bytes}, texture={len(texture)}")
    palette = [rgb555(value) for (value,) in struct.iter_unpack("<H", payload[:palette_bytes])]
    if not palette or max(texture) >= len(palette):
        raise ValueError(
            f"palette has {len(palette)} colors but texture uses index {max(texture) if texture else '-'}"
        )
    image = Image.new("RGBA", (32, 32))
    image.putdata([palette[index] for index in texture])
    return image, {
        "ttga_header_bytes": header_bytes,
        "palette_bytes": palette_bytes,
        "texture_bytes": len(texture),
        "used_indices": len(set(texture)),
        "visible_colors": len(image.getcolors(2048) or []),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm9", type=Path, default=Path("out/raw/arm9_section_0_02000000.bin"))
    parser.add_argument("--entries", type=Path, default=Path("out/raw/archive/entries"))
    parser.add_argument("--output", type=Path, default=Path("out/exported/humanoid_appearances"))
    args = parser.parse_args()

    data = args.arm9.read_bytes()
    table_offset = TABLE_ADDRESS - ARM9_BASE
    args.output.mkdir(parents=True, exist_ok=True)
    decoded: list[dict] = []
    unresolved: list[dict] = []
    for record_index in range(RECORD_COUNT):
        record = data[table_offset + record_index * RECORD_SIZE : table_offset + (record_index + 1) * RECORD_SIZE]
        appearance_id = struct.unpack_from("<H", record, 6)[0]
        actor_id = FIRST_ACTOR_ID + record_index
        if appearance_id == 0xFFFF:
            unresolved.append({"actor_id": actor_id, "reason": "no_appearance_companion"})
            continue
        matches = list(args.entries.glob(f"{appearance_id:04d}.*"))
        if len(matches) != 1:
            unresolved.append({"actor_id": actor_id, "asset": appearance_id, "reason": "asset_not_unique"})
            continue
        try:
            image, metadata = decode(matches[0].read_bytes())
        except ValueError as error:
            unresolved.append({"actor_id": actor_id, "asset": appearance_id, "reason": str(error)})
            continue
        filename = f"actor_{actor_id:03d}_asset_{appearance_id:04d}.png"
        image.save(args.output / filename)
        decoded.append({"actor_id": actor_id, "asset": appearance_id, "png": filename, **metadata})
        print(f"actor {actor_id}: asset {appearance_id} -> {filename}")

    (args.output / "manifest.json").write_text(
        json.dumps({"schema_version": 1, "decoded": decoded, "unresolved": unresolved}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Decoded {len(decoded)} appearance atlases; {len(unresolved)} unresolved/special")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
