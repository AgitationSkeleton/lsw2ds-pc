#!/usr/bin/env python3
"""Evaluate an NSBCA animation's final frame - the assembled pose.

The arena buildables store their PIECES state as the model's own base pose, and
their built state only as the end of a repair animation. The animation names say
so outright: BATurretBrkIdle1, BATurretBrkIdle2, BATurretBrkRepai, where Brk is
broken. So rendering an entity as the player sees it once built means evaluating
the last frame of its BrkRepair animation.

JNT0 layout, worked out from the bytes:

    +0x00  'J' 00 'A' 'C'
    +0x04  u16 frame count          +0x06  u16 node count
    +0x08  u32 flags
    +0x0C  u32, +0x10 u32           value pool offsets
    +0x14  u16[node count]          per-node data offsets, relative to the anim

and each node's entry is:

    u16 flags, u16 node id, then a track descriptor per animated component:
    u16 start frame, u8 end frame, u8 format, u32 offset to the value array

A component is either identity, a single constant, or an array indexed by
frame. Only the final frame is wanted here, so an animated component reduces to
the last element of its array.

Run:
    python tools/nsbca_final_pose.py 339
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract_nsbmd_object_trs import info_block

ROOT = Path(__file__).resolve().parents[1]


def find_jnt0(data: bytes):
    if data[:4] != b"BCA0":
        return None
    for index in range(struct.unpack_from("<H", data, 14)[0]):
        offset = struct.unpack_from("<I", data, 16 + 4 * index)[0]
        if data[offset: offset + 4] == b"JNT0":
            return offset
    return None


def fx32(value: int) -> float:
    return struct.unpack("<i", struct.pack("<I", value & 0xFFFFFFFF))[0] / 4096.0


def fx16(value: int) -> float:
    return struct.unpack("<h", struct.pack("<H", value & 0xFFFF))[0] / 4096.0


def read_track(data: bytes, base: int, cursor: int, frames: int, wide: bool):
    """Return (final value, bytes consumed) for one animated component.

    Track values are fx16, not fx32. Reading them 32 bits wide produced
    translations like -434262.63 where the 16-bit reading of the same bytes
    gives -6.63, which is the scale the models actually work in.
    """
    _start, end_frame, _fmt, offset = struct.unpack_from("<HBBI", data, cursor)
    pool = base + offset
    count = max(1, end_frame or frames)
    last = pool + 2 * (count - 1)
    if last + 2 > len(data):
        return 0.0, 8
    return fx16(struct.unpack_from("<H", data, last)[0]), 8


def final_pose(data: bytes):
    """{node id: translation} at the animation's last frame.

    The flag encoding was solved by fitting every node entry's known byte size
    against its flags across 20,319 samples from every NSBCA in the archive -
    75 distinct flag values, all explained with zero error:

        payload = 56 - 12*bit1 - 4*(bit3+bit4+bit5+bit6+bit8) - 24*bit9

    56 is seven components at eight bytes each (Tx,Ty,Tz,R,Sx,Sy,Sz all
    animated). bit1 stores the translation as three 4-byte constants instead of
    three 8-byte tracks; bits 3, 4 and 5 drop the X, Y and Z components
    entirely; bit9 drops scale. bit0 means the whole node is identity.

    Fitting the sizes rather than guessing the semantics is what settled this
    after two wrong readings.
    """
    base_jnt = find_jnt0(data)
    if base_jnt is None:
        return {}
    animations = info_block(data, base_jnt + 8, 4)
    if not animations:
        return {}
    relative, _name = animations[0]
    anim = base_jnt + relative
    if data[anim: anim + 4] != bytes([0x4A, 0x00, 0x41, 0x43]):
        return {}
    frames, nodes = struct.unpack_from("<HH", data, anim + 4)

    pose = {}
    for index in range(nodes):
        offset = struct.unpack_from("<H", data, anim + 0x14 + 2 * index)[0]
        cursor = anim + offset
        if cursor + 4 > len(data):
            continue
        flags = struct.unpack_from("<H", data, cursor)[0]
        # The node id is big-endian in this field; reading it little-endian
        # gives 0, 256, 512 instead of 0, 1, 2.
        node_id = struct.unpack_from(">H", data, cursor + 2)[0]
        cursor += 4

        translation = [0.0, 0.0, 0.0]
        if not (flags & 0x0001):
            constant = bool(flags & 0x0002)
            for axis in range(3):
                if flags & (0x0008 << axis):
                    continue                    # this axis is not stored
                if constant:
                    if cursor + 4 > len(data):
                        break
                    translation[axis] = fx32(
                        struct.unpack_from("<I", data, cursor)[0])
                    cursor += 4
                else:
                    if cursor + 8 > len(data):
                        break
                    value, used = read_track(data, anim, cursor, frames, True)
                    translation[axis] = value
                    cursor += used
        pose[node_id] = tuple(translation)
    return pose


def main() -> int:
    for argument in sys.argv[1:] or ["339"]:
        asset = int(argument)
        path = ROOT / f"out/raw/archive/entries/{asset:04d}.nsbca"
        if not path.exists():
            print(f"{asset}: missing")
            continue
        data = path.read_bytes()
        pose = final_pose(data)
        print(f"{asset}: {len(pose)} nodes")
        for node_id in sorted(pose)[:20]:
            t = pose[node_id]
            print(f"   node {node_id:2d} t=({t[0]:8.2f},{t[1]:8.2f},{t[2]:8.2f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
