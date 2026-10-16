#!/usr/bin/env python3
"""Inventory every Nitro TEX0 texture in the extracted archive.

Answers "does the ROM contain authored portrait or UI sprites at all" without
converting anything: it walks the texture info list of every BTX0 and BMD0
entry and reports each texture's name, size and format. Purely structural, so
it is fast enough to run over all 5,548 entries.
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"

# Nitro texture formats, index matches texImageParam bits 26..28.
FORMATS = {
    0: "none", 1: "a3i5", 2: "palette4", 3: "palette16",
    4: "palette256", 5: "compressed4x4", 6: "a5i3", 7: "direct",
}


def name_at(data: bytes, offset: int) -> str:
    return data[offset:offset + 16].split(b"\0", 1)[0].decode("ascii", "replace")


def parse_info_list(data: bytes, offset: int) -> tuple[list[bytes], list[str]]:
    """Parse a Nitro G3D info list, returning per-object data and names."""
    count = data[offset + 1]
    # 4-byte header, then the unknown block (u16 header size, u16 section size,
    # u32 constant, then count 4-byte entries), then the datum block, then the
    # 16-byte name table.
    unknown_end = offset + 12 + count * 4
    datum_size = struct.unpack_from("<H", data, unknown_end)[0]
    data_start = unknown_end + 4
    names_start = data_start + count * datum_size
    entries = [
        data[data_start + index * datum_size:data_start + (index + 1) * datum_size]
        for index in range(count)
    ]
    names = [name_at(data, names_start + index * 16) for index in range(count)]
    return entries, names


def find_tex0(data: bytes) -> int | None:
    if data[:4] == b"TEX0":
        return 0
    if data[:4] not in (b"BMD0", b"BTX0"):
        return None
    # Nitro container: u32 magic, u16 bom, u16 version, u32 size, u16 header
    # size, u16 block count, then a table of absolute block offsets.
    block_count = struct.unpack_from("<H", data, 14)[0]
    for index in range(block_count):
        offset = struct.unpack_from("<I", data, 16 + index * 4)[0]
        if offset + 4 <= len(data) and data[offset:offset + 4] == b"TEX0":
            return offset
    return None


def textures_in(data: bytes) -> list[dict]:
    base = find_tex0(data)
    if base is None:
        return []
    try:
        info_offset = struct.unpack_from("<H", data, base + 0x0E)[0]
        entries, names = parse_info_list(data, base + info_offset)
    except (struct.error, IndexError):
        return []
    found = []
    for datum, name in zip(entries, names):
        if len(datum) < 4:
            continue
        param = struct.unpack_from("<I", datum, 0)[0]
        width = 8 << ((param >> 20) & 0x07)
        height = 8 << ((param >> 23) & 0x07)
        found.append({
            "name": name,
            "width": width,
            "height": height,
            "format": FORMATS.get((param >> 26) & 0x07, "unknown"),
        })
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", type=Path, default=DEFAULT_ENTRIES)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    inventory: list[dict] = []
    sizes: Counter[str] = Counter()
    formats: Counter[str] = Counter()
    for path in sorted(args.entries.glob("*")):
        if not path.is_file():
            continue
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if data[:4] not in (b"BMD0", b"BTX0", b"TEX0"):
            continue
        for texture in textures_in(data):
            texture["asset"] = int(path.stem)
            inventory.append(texture)
            sizes[f"{texture['width']}x{texture['height']}"] += 1
            formats[texture["format"]] += 1

    print(f"{len(inventory)} textures across the archive")
    print("\nby size:")
    for size, count in sizes.most_common():
        print(f"  {size:>10}  {count}")
    print("\nby format:")
    for name, count in formats.most_common():
        print(f"  {name:>14}  {count}")

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps({"schema_version": 1, "textures": inventory}, indent=1) + "\n",
            encoding="utf-8",
        )
        print(f"\nWrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
