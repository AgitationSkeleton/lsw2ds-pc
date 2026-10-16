#!/usr/bin/env python3
"""Ask a running melonDS which archive assets are loaded - all of them.

`read_process_memory.py --sprites` only looks for the 202 assets that match the
"sprite triple" heuristic (pixels at n-1, header at n, palette at n+1). That
heuristic is how the HUD hunt kept coming up empty: an element it does not
recognise as a triple is never searched for, so it can be sitting in VRAM in
plain sight and still be reported absent.

This drops the heuristic. Every archive entry of at least 64 bytes becomes a
signature, and the emulator's whole address space is scanned for all of them.
Whatever the touch screen is drawing has to be in there somewhere.

Signatures are taken from the middle of each entry, away from headers that
sibling assets often share, and flat slices are skipped because they would
match anywhere.

MATCHING. Do not do this with `re`. Python's engine does not build an
automaton for an alternation - it retries every branch at every byte, so a few
hundred literal alternatives over a 94 MB region does not finish in any useful
time. Instead each signature's first four bytes become a u32 key; numpy builds
the u32 at every byte offset of the region in one vectorised pass, and
`searchsorted` against the sorted key array finds every candidate at once.
Only those few positions are then verified against the full 32 bytes.

Read-only and never pauses the emulator - safe during a live match.

Run:
    python tools/identify_loaded_assets.py
    python tools/identify_loaded_assets.py --near 0x136bc4ba3d0 --window 0x20000
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_process_memory import Process, find_melonds_pid  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

SIGNATURE_BYTES = 32
MIN_ASSET = 64
SLICE = 8 * 1024 * 1024
# A slice with this little variety would match padding anywhere in memory.
MIN_DISTINCT = 6


def signatures() -> dict[bytes, int]:
    """One distinctive middle slice per archive entry."""
    found: dict[bytes, int] = {}
    for path in sorted(ENTRIES.iterdir()):
        try:
            asset_id = int(path.stem)
        except ValueError:
            continue
        data = path.read_bytes()
        if len(data) < MIN_ASSET:
            continue
        for fraction in (2, 3, 4, 8):
            start = (len(data) // fraction) & ~1
            piece = data[start : start + SIGNATURE_BYTES]
            if len(piece) < SIGNATURE_BYTES:
                continue
            if len(set(piece)) < MIN_DISTINCT:
                continue
            if piece not in found:
                found[piece] = asset_id
            break
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--near", help="only scan around this address")
    parser.add_argument("--window", default="0x40000", help="bytes either side of --near")
    parser.add_argument("--output", type=Path, default=ROOT / "out/work/loaded_assets.json")
    arguments = parser.parse_args()
    if not ENTRIES.exists():
        raise SystemExit("run the ROM extraction step first")

    table = signatures()
    print(f"{len(table)} asset signatures")
    # u32 key -> list of (full signature, asset id) sharing that first word.
    by_key: dict[int, list] = {}
    for piece, asset_id in table.items():
        key = int.from_bytes(piece[:4], "little")
        by_key.setdefault(key, []).append((piece, asset_id))
    keys = numpy.array(sorted(by_key), dtype=numpy.uint32)
    print(f"{len(keys)} distinct 4-byte keys")

    pid = arguments.pid or find_melonds_pid()
    process = Process(pid)
    try:
        if arguments.near:
            centre = int(arguments.near, 0)
            window = int(arguments.window, 0)
            targets = [(centre - window, window * 2)]
        else:
            targets = process.regions()
        print(f"pid {pid}: scanning {len(targets)} regions, "
              f"{sum(s for _, s in targets)/1e6:.0f} MB")

        hits: dict[int, dict] = {}
        for index, (base, size) in enumerate(targets, 1):
            data = process.read(base, size)
            if not data or len(data) < SIGNATURE_BYTES:
                continue
            # Scan in slices so the u32 view stays a bounded amount of memory.
            for start in range(0, len(data), SLICE):
                chunk = data[start : start + SLICE + SIGNATURE_BYTES]
                if len(chunk) < SIGNATURE_BYTES:
                    break
                raw = numpy.frombuffer(chunk, dtype=numpy.uint8)
                words = (raw[:-3].astype(numpy.uint32)
                         | (raw[1:-2].astype(numpy.uint32) << 8)
                         | (raw[2:-1].astype(numpy.uint32) << 16)
                         | (raw[3:].astype(numpy.uint32) << 24))
                where = numpy.searchsorted(keys, words)
                where[where >= len(keys)] = 0
                candidates = numpy.nonzero(keys[where] == words)[0]
                for position in candidates.tolist():
                    key = int(words[position])
                    for piece, asset_id in by_key[key]:
                        if asset_id in hits:
                            continue
                        if chunk[position : position + SIGNATURE_BYTES] == piece:
                            hits[asset_id] = {
                                "asset": asset_id,
                                "address": base + start + position,
                                "region": f"{base:#018x}",
                            }
            print(f"  region {index}/{len(targets)} {base:#018x} "
                  f"{size/1e6:6.1f} MB -> {len(hits)} assets so far", flush=True)

        print(f"\n{len(hits)} archive assets resident:")
        for hit in sorted(hits.values(), key=lambda h: h["address"]):
            path = next(ENTRIES.glob(f"{hit['asset']:04d}.*"), None)
            hit["size"] = path.stat().st_size if path else 0
            hit["name"] = path.name if path else ""
            print(f"  asset {hit['asset']:5d}  {hit['size']:7d} bytes  "
                  f"at {hit['address']:#018x}  {hit['name']}")

        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(
            {"pid": pid, "assets": sorted(hits.values(), key=lambda h: h["address"])},
            indent=2) + "\n", encoding="utf-8")
        print(f"\n-> {arguments.output}")
    finally:
        process.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
