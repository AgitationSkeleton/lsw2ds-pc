"""Export the DS lighting rigs and the per-level index that selects them.

WHERE THIS LIVES IN THE ROM
---------------------------
The game does not compute lighting from a scene description. It carries a table
of fixed rigs and picks one by LEVEL ID.

    0x020FF54C   the index: one 32-bit rig pointer per level id
    0x02093294   the reader:  ldr r1,[pc,#0x48]      -> 0x020FF54C
                              ldr r1,[r1, r0, lsl #2] -> rig = index[levelId]
                              str r1,[r4,#0x5c]
                 then a loop that copies each light's colour halfword out:
                              add  r0, r1, r2, lsl #4     rig + i*16
                              ldrh r1, [r0, #4]           the colour
                              strh r1, [r0(=r4+i*2), #0x60]
                 which is what fixes the record stride at 16 and the colour at
                 +4 inside it.

    0x02093268   cmp r1,#0x33 / beq  and  0x02093288 cmp r1,#9 / bne
    0x02093290   mov r0,#0x47                a level-id override before the
                                             lookup - id 0x47 for two cases

RIG LAYOUT (confirmed against the reader and against captured registers)

    +0x00  u8    light count            0x020932A8  ldrb r0,[r1]
    +0x02  u16   MAT_AMBIENT, rgb15
    +0x04  }
    +0x08  }     light 0: u32 colour rgb15, then s32 x, y, z in 20.12
    +0x0C  }
    +0x10  }
    ... 16 bytes per light thereafter

The rig for a level is NOT a snapshot of one frame's registers - it is the
authored source those registers are loaded from, and it is exact.

THE SIGN OF THE DIRECTION
-------------------------
The stored vector is the direction the light TRAVELS. The hardware negates it
when the LIGHT_VECTOR command is issued - melonDS GPU3D.cpp:2045 is

    LightDirection[l][k] = -((dir . VecMatrix column k) >> 12)

and the vertex stage at :1492 then dots that stored value with the transformed
normal WITHOUT negating again. So the shading term is

    lambert_i = max(0, dot(-dir_i, N))

with dir_i straight out of this table. Captured frames confirm it: in frames
where the vector matrix is identity the register reads exactly -(dir >> 3),
e.g. light 0 of the arena rig reads (308, 205, 359) against a table entry of
(-2458, -1638, -2867) with -2458 >> 3 = -308.

The hardware only ever sees the 10-bit form, so the >> 3 column below is what
is actually rendered and is what the port must use.

THE EXACT INTENSITY
-------------------
GPU3D.cpp:1500-1542, per channel and per enabled light:

    diffuse   += (MatDiffuse * LightColor * diffdot) & 0xFFFFF     diffdot <= 512
    ambient   += (MatAmbient << 9) * LightColor
    result5    = min(31, (emission << 14 + sum) >> 14)

so with everything at 31 a head-on light gives 31*31*512 >> 14 = 30, not 31.
The scale is exactly 31/32 = 0.96875 against a naive float product, and it
applies to the ambient term too. Written out for a shader working in 0..1:

    shade = clamp(emi5/31 + sum_i (dif5*col5_i*lambert_i + amb5*col5_i)/(32*31))

Usage:  python tools/export_lighting_rigs.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000

INDEX = 0x020FF54C
# The rigs themselves sit in one contiguous run at 0x34 apart. These bounds are
# the first and last addresses any index entry points at, widened to the stride.
RIG_LO = 0x020F4D80
RIG_HI = 0x020F5100
RIG_STRIDE = 0x34

# 0x0209E404 accepts a level into the arena list only when its class byte is
# 0x11, and 0x0209E410 / 0x0209E41C name the two that are.
ARENA_LEVELS = {4: "jabbas_tower", 5: "mos_eisley_streets"}

OUT = ROOT / "config" / "lighting_rigs.json"


def rgb5(word: int) -> list[int]:
    return [word & 0x1F, (word >> 5) & 0x1F, (word >> 10) & 0x1F]


def main() -> int:
    image = IMAGE.read_bytes()

    def u8(addr: int) -> int:
        return image[addr - BASE]

    def u16(addr: int) -> int:
        return struct.unpack_from("<H", image, addr - BASE)[0]

    def u32(addr: int) -> int:
        return struct.unpack_from("<I", image, addr - BASE)[0]

    def s32(addr: int) -> int:
        return struct.unpack_from("<i", image, addr - BASE)[0]

    # --- the index -------------------------------------------------------
    # Walk until an entry stops pointing into the rig block. 0x02093290's
    # `mov r0,#0x47` proves the table reaches at least id 0x47.
    index: dict[int, int] = {}
    level = 0
    while True:
        value = u32(INDEX + level * 4)
        if not (RIG_LO <= value < RIG_HI):
            break
        index[level] = value
        level += 1
        if level > 0x100:
            break

    # --- every rig any level points at -----------------------------------
    rigs: dict[int, dict] = {}
    for pointer in sorted(set(index.values())):
        count = u8(pointer)
        lights = []
        for i in range(count):
            record = pointer + 4 + i * 16
            colour = u32(record)
            direction = [s32(record + 4), s32(record + 8), s32(record + 12)]
            lights.append({
                "colour_rgb5": rgb5(colour),
                # 20.12 as authored.
                "direction_fixed_20_12": direction,
                # >> 3 into the 10-bit form the LIGHT_VECTOR command carries,
                # which is what the hardware actually shades with.
                "direction_fixed_1_0_9": [v >> 3 for v in direction],
                "direction": [(v >> 3) / 512.0 for v in direction],
            })
        rigs[pointer] = {
            "address": "0x%08X" % pointer,
            "light_count": count,
            "mat_ambient_rgb5": rgb5(u16(pointer + 2)),
            "lights": lights,
        }

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "index_table": "0x%08X" % INDEX,
            "reader": "0x02093294",
            "record_stride": 16,
            "rig_stride": RIG_STRIDE,
        },
        "note": __doc__.strip(),
        "index_by_level": {str(k): "0x%08X" % v for k, v in sorted(index.items())},
        "rigs": {"0x%08X" % k: v for k, v in sorted(rigs.items())},
        "arenas": {
            name: {
                "level_id": level_id,
                "rig": "0x%08X" % index[level_id],
            }
            for level_id, name in ARENA_LEVELS.items()
            if level_id in index
        },
    }

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=2), encoding="utf-8")

    print("index entries: %d (levels 0..%d)" % (len(index), max(index)))
    print("distinct rigs: %d" % len(rigs))
    for level_id, name in sorted(ARENA_LEVELS.items()):
        pointer = index.get(level_id)
        if pointer is None:
            print("  level %d (%s): NOT IN INDEX" % (level_id, name))
            continue
        rig = rigs[pointer]
        print("  level %d %-20s -> rig 0x%08X  ambient rgb5 %s" % (
            level_id, name, pointer, rig["mat_ambient_rgb5"]))
        for i, light in enumerate(rig["lights"]):
            print("      light%d colour %-12s dir %-24s (10-bit %s)" % (
                i, light["colour_rgb5"],
                [round(v, 6) for v in light["direction"]],
                light["direction_fixed_1_0_9"]))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
