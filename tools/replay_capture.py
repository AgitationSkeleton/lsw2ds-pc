#!/usr/bin/env python3
"""Replay a recorded session: reconstruct memory at any captured frame.

The patched melonDS records a play session continuously. Main RAM is 4 MB and
sub-engine VRAM another 144 KB, which cannot be written six times a second - so
each capture stores only the **blocks that changed** since the previous one,
with a full keyframe periodically. This walks that chain and hands back the
exact bytes at any frame.

    f000000_ram.key       full 4 MB snapshot
    f000010_ram.delta     "DLTA", block size, count, then [offset][len][bytes]
    f000010_vramC.delta   sub BG  - tilemaps, score digits, radar sweep
    f000010_vramI.delta   sub OBJ - sprite tile pixels
    f000010_state.json    2D registers and decoded OAM
    f000010_palette.bin   palette RAM

Because the whole session is on disk, an event can be located *after the fact*:
find the frame where a score changed, then read the memory around it. That is
what makes finding match state possible without stopping to take snapshots.

Run:
    python tools/replay_capture.py --dir out/work/gpudump/inst0 --list
    python tools/replay_capture.py --dir out/work/gpudump/inst0 --frame 12000 \\
        --out out/work/frame12000
    python tools/replay_capture.py --dir out/work/gpudump/inst0 --watch 0x02109250
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_RAM_BASE = 0x02000000
REGIONS = {"ram": 4 * 1024 * 1024, "vramC": 128 * 1024, "vramI": 16 * 1024}


def frames_available(directory: Path, region: str) -> list[int]:
    numbers = set()
    for path in directory.glob(f"f*_{region}.*"):
        if path.suffix not in (".key", ".delta"):
            continue
        numbers.add(int(path.stem.split("_")[0][1:]))
    return sorted(numbers)


def apply_delta(buffer: bytearray, path: Path) -> None:
    data = path.read_bytes()
    if data[:4] != b"DLTA":
        raise SystemExit(f"{path.name} is not a delta file")
    _, count = struct.unpack_from("<II", data, 4)
    offset = 12
    for _ in range(count):
        position, length = struct.unpack_from("<II", data, offset)
        offset += 8
        buffer[position : position + length] = data[offset : offset + length]
        offset += length


def reconstruct(directory: Path, region: str, target: int) -> bytearray:
    """Walk forward from the last keyframe at or before `target`."""
    size = REGIONS[region]
    numbers = frames_available(directory, region)
    numbers = [n for n in numbers if n <= target]
    if not numbers:
        raise SystemExit(f"no {region} capture at or before frame {target}")

    start = None
    for number in reversed(numbers):
        if (directory / f"f{number:06d}_{region}.key").exists():
            start = number
            break
    if start is None:
        raise SystemExit(f"no {region} keyframe at or before frame {target} - "
                         "the recording may have begun mid-session")

    buffer = bytearray((directory / f"f{start:06d}_{region}.key").read_bytes())
    if len(buffer) < size:
        buffer.extend(bytes(size - len(buffer)))
    for number in numbers:
        if number <= start:
            continue
        delta = directory / f"f{number:06d}_{region}.delta"
        if delta.exists():
            apply_delta(buffer, delta)
    return buffer


def cmd_watch(directory: Path, address: int, width: int, limit: int) -> None:
    """Report every frame at which a given address changed value."""
    region, base = "ram", MAIN_RAM_BASE
    offset = address - base
    if not 0 <= offset < REGIONS[region]:
        raise SystemExit(f"{address:#x} is outside main RAM")

    numbers = frames_available(directory, region)
    fmt = {1: "<B", 2: "<H", 4: "<I"}[width]
    buffer = None
    previous = None
    changes = 0
    for number in numbers:
        key = directory / f"f{number:06d}_{region}.key"
        delta = directory / f"f{number:06d}_{region}.delta"
        if key.exists():
            buffer = bytearray(key.read_bytes())
        elif buffer is not None and delta.exists():
            apply_delta(buffer, delta)
        else:
            continue
        value = struct.unpack_from(fmt, buffer, offset)[0]
        if value != previous:
            print(f"  f{number:<8} {address:#010x} = {value}")
            previous = value
            changes += 1
            if changes >= limit:
                print("  ... (limit reached)")
                return
    if not changes:
        print("  never changed across the recording")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--frame", type=int)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--watch", help="address to trace, e.g. 0x02109250")
    parser.add_argument("--width", type=int, default=2, choices=[1, 2, 4])
    parser.add_argument("--limit", type=int, default=60)
    arguments = parser.parse_args()

    if arguments.list:
        for region in REGIONS:
            numbers = frames_available(arguments.dir, region)
            keys = [n for n in numbers
                    if (arguments.dir / f"f{n:06d}_{region}.key").exists()]
            if numbers:
                print(f"{region:6}: {len(numbers)} captures, {len(keys)} keyframes, "
                      f"frames {numbers[0]}..{numbers[-1]}")
            else:
                print(f"{region:6}: none")
        return 0

    if arguments.watch:
        print(f"tracing {arguments.watch} as u{arguments.width * 8}:")
        cmd_watch(arguments.dir, int(arguments.watch, 0), arguments.width,
                  arguments.limit)
        return 0

    if arguments.frame is None:
        parser.error("give --list, --frame or --watch")

    out = arguments.out or (arguments.dir / f"frame{arguments.frame}")
    out.mkdir(parents=True, exist_ok=True)
    for region in REGIONS:
        try:
            buffer = reconstruct(arguments.dir, region, arguments.frame)
        except SystemExit as error:
            print(f"{region}: {error}")
            continue
        name = "mainram.bin" if region == "ram" else f"vram_{region[-1]}.bin"
        (out / name).write_bytes(buffer)
        print(f"{region}: {len(buffer):,} bytes -> {name}")

    state = arguments.dir / f"f{arguments.frame:06d}_state.json"
    if state.exists():
        (out / "state.json").write_text(state.read_text(encoding="utf-8"),
                                        encoding="utf-8")
        print("state.json copied")
    palette = arguments.dir / f"f{arguments.frame:06d}_palette.bin"
    if palette.exists():
        (out / "palette.bin").write_bytes(palette.read_bytes())
        print("palette.bin copied")
    print(f"-> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
