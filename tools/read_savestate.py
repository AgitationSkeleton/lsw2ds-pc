#!/usr/bin/env python3
"""Read a DeSmuME savestate and report which archive assets are resident.

This is the answer to "which texture is this screen actually using". Rather
than guessing from pixels, take a savestate while the screen of interest is
displayed and ask the machine what it loaded.

HOW IT WORKS

A DeSmuME `.dsN` state is a 32-byte header (`DeSmuME SState`, version, sizes)
followed by one zlib stream. Decompressed it contains main RAM, VRAM and the
rest of the machine. Main RAM is located by matching a signature taken from the
extracted ARM9 image, which the loader places at 0x02000000 - two distant
signatures are checked so a chance match cannot mislocate it.

Residency is then decided by searching the decompressed state for each sprite's
pixel bytes. Sprite graphics are uploaded to VRAM, not main RAM, so the search
covers the whole state rather than just the 4MB window.

WHAT IT IS FOR

Run it on a state captured on the Bounty Hunter touchscreen and it names the
hearts, the skulls, the radar and the carbonite marker outright - the assets
this project has otherwise failed to find by inspection. Verified on the
"Leia at the arena console" state, where it correctly reports exactly four
resident sprites: Leia's portrait (3512), the second party portrait (4151),
the blue portrait ring (621) and a 16x16 status icon (5253).

Both DeSmuME (`.dsN`, zlib) and melonDS (`.mlN`, `MELN`, uncompressed) states
are handled.

Run:
    python tools/read_savestate.py "path/to/state.ds1"
    python tools/read_savestate.py state.ds1 --dump-ram out/work/ram.bin
"""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"
ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"

MAGIC = b"DeSmuME SState"
MELON_MAGIC = b"MELN"
MAIN_RAM_BASE = 0x02000000
MAIN_RAM_SIZE = 4 * 1024 * 1024
# Signature offsets into the ARM9 image; the second confirms the first.
PROBE_OFFSETS = (0x1000, 0x40000, 0x80000)
CONFIRM_OFFSET = 0xC0000
SIGNATURE_BYTES = 64
# A slice this flat is probably padding and would match anywhere.
FLAT_LIMIT = 40


def load_state(path: Path) -> bytes:
    """Return the raw machine image from a DeSmuME or melonDS savestate."""
    data = path.read_bytes()
    if data.startswith(MELON_MAGIC):
        # melonDS: MELN, u16 major, u16 minor, u32 length, then uncompressed
        # sections of [magic][length][reserved][reserved]. Nothing to inflate.
        major, minor = struct.unpack_from("<HH", data, 4)
        print(f"melonDS savestate v{major}.{minor}, uncompressed")
        return data
    if data.startswith(MAGIC):
        for offset in range(16, 64):
            if data[offset : offset + 2] == b"\x78\x9c":
                return zlib.decompress(data[offset:])
        raise SystemExit("no zlib stream found in the DeSmuME savestate")
    raise SystemExit(f"{path.name} is neither a DeSmuME nor a melonDS savestate")


def melon_sections(data: bytes) -> list[tuple[str, int, int]]:
    """Section magics in a melonDS state, for orientation."""
    sections, offset = [], 0x10
    while offset + 16 <= len(data):
        magic = data[offset : offset + 4]
        length = struct.unpack_from("<I", data, offset + 4)[0]
        if not all(32 <= c < 127 for c in magic) or length < 16:
            break
        if offset + length > len(data):
            break
        sections.append((magic.decode(), length, offset))
        offset += length
    return sections


def locate_main_ram(state: bytes, arm9: bytes) -> int | None:
    for probe in PROBE_OFFSETS:
        signature = arm9[probe : probe + SIGNATURE_BYTES]
        if len(signature) < SIGNATURE_BYTES or signature.count(0) > FLAT_LIMIT:
            continue
        found = state.find(signature)
        if found < 0:
            continue
        base = found - probe
        confirm = arm9[CONFIRM_OFFSET : CONFIRM_OFFSET + SIGNATURE_BYTES]
        if state.find(confirm) == base + CONFIRM_OFFSET:
            return base
    return None


def read_asset(asset_id: int) -> bytes | None:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    return matches[0].read_bytes() if matches else None


def sprite_triples() -> dict[int, dict]:
    sizes = {int(path.stem): path.stat().st_size for path in ENTRIES.iterdir()}
    found: dict[int, dict] = {}
    for asset_id, size in sizes.items():
        if not 8 <= size <= 4096:
            continue
        header = read_asset(asset_id)
        if header is None or len(header) < 10:
            continue
        declared = struct.unpack_from("<H", header, 8)[0]
        if declared < 32:
            continue
        for gap in (1, 2):
            if sizes.get(asset_id - gap) != declared:
                continue
            if sizes.get(asset_id + 1) not in (32, 512):
                continue
            found[asset_id - gap] = {
                "header": asset_id,
                "palette": asset_id + 1,
                "display": [header[0], header[1]],
                "pixel_bytes": declared,
            }
            break
    return found


def resident(state: bytes, sprites: dict[int, dict]) -> list[dict]:
    hits = []
    for asset_id, info in sorted(sprites.items()):
        pixels = read_asset(asset_id)
        if pixels is None or len(pixels) < SIGNATURE_BYTES:
            continue
        for start in (0, len(pixels) // 3, len(pixels) // 2):
            piece = pixels[start : start + 48]
            if len(piece) < 48 or piece.count(piece[0]) > FLAT_LIMIT:
                continue
            position = state.find(piece)
            if position >= 0:
                hits.append({
                    "asset": asset_id,
                    "header": info["header"],
                    "palette": info["palette"],
                    "display": info["display"],
                    "state_offset": position - start,
                })
                break
    return hits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("state", type=Path)
    parser.add_argument("--dump-ram", type=Path, help="write main RAM to this file")
    parser.add_argument("--dump-state", type=Path, help="write the whole decompressed state")
    parser.add_argument("--output", type=Path, help="write the report as JSON")
    arguments = parser.parse_args()
    if not ENTRIES.exists() or not ARM9.exists():
        raise SystemExit("run the ROM extraction step first")

    state = load_state(arguments.state)
    print(f"{arguments.state.name}: {len(state)/1e6:.1f} MB")
    if state.startswith(MELON_MAGIC):
        sections = melon_sections(state)
        print("sections: " + ", ".join(f"{m}({l})" for m, l, _ in sections[:12]))

    arm9 = ARM9.read_bytes()
    base = locate_main_ram(state, arm9)
    if base is None:
        print("could not locate main RAM (two-signature check failed)")
    else:
        print(f"main RAM at state offset {base} (= 0x{MAIN_RAM_BASE:08x}), confirmed")
        if arguments.dump_ram:
            arguments.dump_ram.parent.mkdir(parents=True, exist_ok=True)
            arguments.dump_ram.write_bytes(state[base : base + MAIN_RAM_SIZE])
            print(f"  -> {arguments.dump_ram}")
    if arguments.dump_state:
        arguments.dump_state.parent.mkdir(parents=True, exist_ok=True)
        arguments.dump_state.write_bytes(state)
        print(f"  -> {arguments.dump_state}")

    sprites = sprite_triples()
    hits = resident(state, sprites)
    print(f"\n{len(hits)} of {len(sprites)} sprite assets are resident on this screen:")
    for hit in sorted(hits, key=lambda h: h["state_offset"]):
        width, height = hit["display"]
        print(f"  asset {hit['asset']:5d}  display {width:3}x{height:<3}  "
              f"palette {hit['palette']:5d}  state offset {hit['state_offset']}")

    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps({
            "state": str(arguments.state),
            "main_ram_offset": base,
            "resident_sprites": hits,
        }, indent=2) + "\n", encoding="utf-8")
        print(f"\n-> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
