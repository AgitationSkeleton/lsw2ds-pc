#!/usr/bin/env python3
"""Export the animation descriptor sets straight out of the ARM9 image.

Everything here is read from decompiled code, not inferred from the data:

  0x020EB7B8   the descriptor-set table, 8-byte records
                 +0  pointer to the descriptor array
                 +4  tag (halfword)
                 +6  count (halfword, compared with a SIGNED ble at 0x02010C20)

  0x02010C0C   the resolver. A linear scan over `count` entries, stride 10,
               matching the requested state against the descriptor's FIRST
               halfword, returning array + index*10. No hashing, no ordering
               assumption - a state absent from the set simply returns null and
               the caller leaves the animation alone.

  entity+0x180 selects the set; 0x020107C8 (vtable slot +0x24) does
               `ldrh r0,[entity+0x180]` then `add r0, 0x020EB7B8, r0 lsl #3`.

The ten-byte descriptor's five halfwords are named for what CONSUMES them, each
confirmed by disassembly rather than guessed from the values:

  +0  state    written to subObj+0xAE (= actor+0x182) by 0x0209F760.
               The band 0x78..0xA0 is special: 0x0209F658 routes it to a LAYER
               channel instead and never writes +0x182 at all.

  +2  anim     the clip asset id, passed as r1 to the clip loader 0x0209F1B0
               (0x0209F800 on the base path, 0x0209F6B4 on the layered path).

  +4  attached an attached sub-object id. 0x02010A24 compares it with the live
               id at [actor+0x190]+0x28 and RETURNS if unchanged; otherwise it
               tears the old one down (0x0205C8B0) and starts the new one with
               (obj, actor, id, actor+0x8C matrix, current frame) and sets bit
               0x200 in actor+0x1A8. Because it only restarts on a change, a
               value repeated across a set's states is a prop that persists
               across them, not an effect retriggered every state.

  +6  events   an event/marker track, loaded by 0x0205AB1C and stored at
               subObj+0xA0. 0x0209F98C reads it as 6-byte records whose first
               halfword is a FRAME number, with the cursor byte at subObj+0xB1.

  +8  queued   an asset queued on the global at 0x027E03A0 via 0x02061194 and
               stored at subObj+0xB4. NOTE: this field is 0xFFFF in all 2468
               descriptors in this cartridge, so the path is dead data here.

  0xFFFF is the "none" sentinel for +4, +6 and +8 (the literal at 0x0209F83C,
  loaded for the +6 and +8 tests, and the early-out at 0x02010A24 for +4).
"""

import argparse
import json
import struct
from pathlib import Path

ARM9_BASE = 0x02000000
SET_TABLE = 0x020EB7B8
# 630, not 256. The first export ASSUMED 256 and silently dropped 374 sets.
#
# The real extent is measurable: records stay plausible (an array pointer inside
# the image, a positive count) through index 629 and index 630 is the first that
# is not - its "pointer" is 0x03F501E0, outside the cartridge entirely.
#
# That boundary is confirmed from the other side by live RAM. Actors that are
# parked with no animation set report entity+0x180 = 630 exactly, i.e. one past
# the end is the engine's own "no set" value.
SET_COUNT = 630
SET_STRIDE = 8
DESCRIPTOR_STRIDE = 10
NONE = 0xFFFF

# The layer bands, from 0x0209F668-0x0209F67C:
#     r4 = 3; if state < 0x8A: r4 = 2; elif state >= 0x9C: r4 = 4
# and the whole band only applies when 0x78 <= state <= 0xA0.
LAYER_LOW = 0x78
LAYER_HIGH = 0xA0


def layer_channel(state: int):
    """The animation channel a state plays on, or None for the base channel."""
    if not (LAYER_LOW <= state <= LAYER_HIGH):
        return None
    if state < 0x8A:
        return 2
    if state >= 0x9C:
        return 4
    return 3


class Image:
    def __init__(self, path: Path, base: int):
        self.data = path.read_bytes()
        self.base = base

    def _off(self, addr: int) -> int:
        off = addr - self.base
        if off < 0 or off >= len(self.data):
            raise IndexError("0x%08X outside the image" % addr)
        return off

    def u16(self, addr: int) -> int:
        return struct.unpack_from("<H", self.data, self._off(addr))[0]

    def u32(self, addr: int) -> int:
        return struct.unpack_from("<I", self.data, self._off(addr))[0]


def export(image: Image) -> dict:
    sets = []
    for index in range(SET_COUNT):
        record = SET_TABLE + index * SET_STRIDE
        array = image.u32(record)
        tag = image.u16(record + 4)
        count = struct.unpack_from(
            "<h", image.data, image._off(record + 6))[0]
        entry = {
            "index": index,
            "array": "0x%08X" % array,
            "tag": "0x%04X" % tag,
            "count": count,
            "descriptors": [],
        }
        # The count is compared with a SIGNED ble, so a negative count means the
        # resolver skips the loop entirely and returns null. Reproduce that
        # rather than reading garbage.
        if count > 0 and array:
            for slot in range(count):
                at = array + slot * DESCRIPTOR_STRIDE
                state = image.u16(at)
                descriptor = {
                    "state": state,
                    "anim": image.u16(at + 2),
                    "attached": image.u16(at + 4),
                    "events": image.u16(at + 6),
                    "queued": image.u16(at + 8),
                }
                channel = layer_channel(state)
                if channel is not None:
                    descriptor["layer"] = channel
                entry["descriptors"].append(descriptor)
        sets.append(entry)

    return {
        "table": {
            "address": "0x%08X" % SET_TABLE,
            "count": SET_COUNT,
            "stride": SET_STRIDE,
            "record": "+0 array pointer, +4 tag, +6 count (signed halfword)",
            "selector": "entity+0x180, indexed by 0x020107C8",
        },
        "descriptor": {
            "stride": DESCRIPTOR_STRIDE,
            "none": NONE,
            "fields": {
                "+0 state": "written to actor+0x182 by 0x0209F760",
                "+2 anim": "clip asset id, loaded by 0x0209F1B0",
                "+4 attached": "sub-object on actor+0x190, restarted only on change (0x02010A24)",
                "+6 events": "frame-tagged event track at subObj+0xA0, 6-byte records (0x0209F98C)",
                "+8 queued": "queued on the global 0x027E03A0 by 0x02061194; unused in this cartridge",
            },
        },
        "resolver": {
            "address": "0x02010C0C",
            "method": "linear scan, stride 10, match on the first halfword",
            "miss": "returns null; the caller leaves the current animation alone",
        },
        "layers": {
            "band": "0x%02X..0x%02X" % (LAYER_LOW, LAYER_HIGH),
            "split": "0x0209F668: channel 2 below 0x8A, 3 below 0x9C, else 4",
            "note": "layered states do NOT write actor+0x182 and do NOT run the "
                    "attached-object path at 0x02010A24",
        },
        "sets": sets,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--arm9",
        default="out/raw/arm9_section_0_02000000.bin",
        help="the decompressed ARM9 section loaded at 0x02000000")
    parser.add_argument("--base", type=lambda v: int(v, 0), default=ARM9_BASE)
    parser.add_argument("--out", default="config/animation_descriptor_sets.json")
    args = parser.parse_args()

    image = Image(Path(args.arm9), args.base)
    document = export(image)

    total = sum(len(s["descriptors"]) for s in document["sets"])
    layered = sum(1 for s in document["sets"]
                  for d in s["descriptors"] if "layer" in d)
    populated = sum(1 for s in document["sets"] if s["descriptors"])

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=1) + "\n")

    print("%s: %d sets (%d populated), %d descriptors, %d layered"
          % (out, len(document["sets"]), populated, total, layered))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
