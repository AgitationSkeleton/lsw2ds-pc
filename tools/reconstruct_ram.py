#!/usr/bin/env python3
"""Rebuild main RAM at a given capture frame from a keyframe plus deltas.

GPUDump.cpp writes main RAM as a full `<frame>_ram.key` whenever the size
changes (in practice, the first capture) and `<frame>_ram.delta` for every
capture after it. A delta is:

    "DLTA" | block size u32 | block count u32 | then per block:
    offset u32 | length u32 | bytes

and each block is applied in order, so replaying every delta up to and
including the requested frame reproduces that frame's RAM exactly.

This exists because runtime values - the respawn locator list, match state,
score, the carbonite carrier - are not in the ARM9 image at all. They only
exist in RAM, and the recorded sessions are the only way to read them without
re-recording.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

MAIN_RAM_BASE = 0x02000000
FRAME_RE = re.compile(r"^f(\d+)_ram\.(key|delta)$")
DTCM_BASE = 0x027E0000
DTCM_BYTES = 16 * 1024


def captures(directory: Path, region: str = "ram"):
    """Every (frame, kind, path) for a region, in frame order.

    THE REGION ARGUMENT EXISTS BECAUSE MAIN RAM IS NOT ENOUGH. The recompiled
    core loads a captured frame and calls the game's own per-frame driver; it
    walks off a pointer three instructions in, because FUN_020016C0 pops from a
    stack whose depth lives at 0x027E02A0 - in DTCM, which no capture recorded.
    The instrumentation now writes f<N>_dtcm.key/.delta in the same format, so
    the same replay works for it.
    """
    pattern = re.compile(r"^f(\d+)_" + region + r"[.](key|delta)$")
    found = []
    for path in directory.iterdir():
        match = pattern.match(path.name)
        if match:
            found.append((int(match.group(1)), match.group(2), path))
    found.sort(key=lambda item: (item[0], 0 if item[1] == "key" else 1))
    return found


def apply_delta(ram: bytearray, path: Path) -> int:
    blob = path.read_bytes()
    if blob[:4] != b"DLTA":
        raise ValueError("%s is not a delta" % path)
    _block, count = struct.unpack_from("<II", blob, 4)
    at = 12
    for _ in range(count):
        offset, length = struct.unpack_from("<II", blob, at)
        at += 8
        ram[offset:offset + length] = blob[at:at + length]
        at += length
    return count


def rebuild(directory: Path, frame: int,
            region: str = "ram") -> tuple[bytearray, int]:
    entries = captures(directory, region)
    if not entries:
        raise SystemExit("no *_%s.key / *_%s.delta in %s"
                         % (region, region, directory))

    # Start from the LAST keyframe at or before the requested frame - replaying
    # from an earlier one would be correct but slower.
    start = None
    for index, (number, kind, path) in enumerate(entries):
        if kind == "key" and number <= frame:
            start = index
    if start is None:
        raise SystemExit("no keyframe at or before frame %d" % frame)

    ram = bytearray(entries[start][2].read_bytes())
    reached = entries[start][0]
    for number, kind, path in entries[start + 1:]:
        if number > frame:
            break
        if kind == "key":
            ram = bytearray(path.read_bytes())
        else:
            apply_delta(ram, path)
        reached = number
    return ram, reached


class Ram:
    def __init__(self, data: bytearray, base: int = MAIN_RAM_BASE):
        self.data = data
        self.base = base

    def _off(self, addr: int) -> int:
        off = addr - self.base
        if off < 0 or off >= len(self.data):
            raise IndexError("0x%08X outside the %d-byte image" % (addr, len(self.data)))
        return off

    def u8(self, addr): return self.data[self._off(addr)]
    def u16(self, addr): return struct.unpack_from("<H", self.data, self._off(addr))[0]
    def s16(self, addr): return struct.unpack_from("<h", self.data, self._off(addr))[0]
    def u32(self, addr): return struct.unpack_from("<I", self.data, self._off(addr))[0]
    def s32(self, addr): return struct.unpack_from("<i", self.data, self._off(addr))[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="a gpudump instance directory")
    parser.add_argument("frame", type=int, help="the capture frame to rebuild")
    parser.add_argument("--out", help="write the rebuilt RAM here")
    parser.add_argument("--peek", action="append", default=[],
                        help="ADDR[:u8|u16|s16|u32|s32|hexN] to print")
    parser.add_argument("--list", action="store_true",
                        help="list the available frames and exit")
    args = parser.parse_args()

    directory = Path(args.directory)
    if args.list:
        entries = captures(directory)
        keys = [n for n, kind, _ in entries if kind == "key"]
        print("%d captures, frames %d..%d, %d keyframe(s): %s"
              % (len(entries), entries[0][0], entries[-1][0], len(keys), keys[:8]))
        return 0

    ram, reached = rebuild(directory, args.frame)
    print("rebuilt frame %d (%d bytes)" % (reached, len(ram)), file=sys.stderr)
    image = Ram(ram)

    for spec in args.peek:
        addr_text, _, kind = spec.partition(":")
        addr = int(addr_text, 0)
        kind = kind or "u32"
        if kind.startswith("hex"):
            count = int(kind[3:] or 16)
            blob = ram[addr - MAIN_RAM_BASE: addr - MAIN_RAM_BASE + count]
            print("0x%08X  %s" % (addr, blob.hex(" ")))
        else:
            print("0x%08X  %s = %d (0x%X)"
                  % (addr, kind, getattr(image, kind)(addr), getattr(image, kind)(addr)))

    if args.out:
        Path(args.out).write_bytes(bytes(ram))
        print("wrote %s" % args.out, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
