#!/usr/bin/env python3
"""Rebuild any GPUDump blob - vramC, vramI, main RAM - at a given frame.

reconstruct_ram.py only handles `f<frame>_ram.*`. The same keyframe-plus-delta
scheme is used for every blob GPUDump writes, and the sub-BG data in vramC is
where the tilemaps, score digits and radar panel live - none of which are
visible in OAM, so an OAM-only comparison cannot see them.

Blob kinds seen in these captures: ram, vramC (sub BG), vramI (sub OBJ tiles).
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


def captures(directory: Path, kind: str):
    pattern = re.compile(r"^f(\d+)_%s\.(key|delta)$" % re.escape(kind))
    found = []
    for path in directory.iterdir():
        match = pattern.match(path.name)
        if match:
            found.append((int(match.group(1)), match.group(2), path))
    found.sort(key=lambda item: (item[0], 0 if item[1] == "key" else 1))
    return found


def apply_delta(blob: bytearray, path: Path) -> None:
    data = path.read_bytes()
    if data[:4] != b"DLTA":
        raise ValueError("%s is not a delta" % path)
    _block, count = struct.unpack_from("<II", data, 4)
    at = 12
    for _ in range(count):
        offset, length = struct.unpack_from("<II", data, at)
        at += 8
        blob[offset:offset + length] = data[at:at + length]
        at += length


def rebuild(directory: Path, frame: int, kind: str = "vramC"):
    entries = captures(directory, kind)
    if not entries:
        raise SystemExit("no %s captures in %s" % (kind, directory))
    start = None
    for index, (number, form, _) in enumerate(entries):
        if form == "key" and number <= frame:
            start = index
    if start is None:
        raise SystemExit("no %s keyframe at or before %d" % (kind, frame))
    blob = bytearray(entries[start][2].read_bytes())
    reached = entries[start][0]
    for number, form, path in entries[start + 1:]:
        if number > frame:
            break
        if form == "key":
            blob = bytearray(path.read_bytes())
        else:
            apply_delta(blob, path)
        reached = number
    return blob, reached


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("frame", type=int)
    parser.add_argument("--kind", default="vramC")
    parser.add_argument("--out")
    args = parser.parse_args()
    blob, reached = rebuild(Path(args.directory), args.frame, args.kind)
    print("rebuilt %s at frame %d (%d bytes)" % (args.kind, reached, len(blob)))
    if args.out:
        Path(args.out).write_bytes(bytes(blob))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
