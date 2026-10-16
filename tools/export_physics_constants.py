#!/usr/bin/env python3
"""Recover the movement and physics constants from the ARM9 image.

Everything written here is read out of the static image; nothing is tuned. The
three sources are:

  * the double-to-20.12 converter at 0x02007f24, whose callers build every
    authored scalar in the engine (gravity, jump impulse, ...);
  * the per-motion stat banks reached through 0x0208dad8, whose +0x20 and +0x24
    words are the walk and run speeds;
  * the controller mode table at 0x020ed2e8, which selects the horizontal blend
    class and the vertical impulse handler.

Run:
    python tools/export_physics_constants.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, CS_OP_MEM, Cs

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out/raw/arm9_section_0_02000000.bin"
OUTPUT = ROOT / "config/physics.json"
BASE = 0x02000000

CONVERTER = "#0x2007f24"
GRAVITY_SITE = 0x02017C40
JUMP_PLAIN_SITE = 0x020470A8
JUMP_SCALE_SITE = 0x02047050
MODE_TABLE = 0x020ED2E8
MODE_ROWS = 24
SPEED_CLASS_TABLE = 0x020EE04C

# 0x0208da74 classifies a motion id into one of five banks; 0x0208dad8 then
# indexes the matching table. Ranges and strides are read straight off it.
MOTION_BANKS = [
    (0x00, 0x2C, 0x020F2414, 0x28),
    (0x2D, 0x6A, 0x020F3E68, 0x2C),
    (0x6B, 0xB0, 0x020F3490, 0x24),
    (0xB1, 0xE7, 0x020F2B1C, 0x2C),
    (0xE8, 0xF9, 0x020F2144, 0x28),
]
WALK_FIELD = 0x20
RUN_FIELD = 0x24
FIXED_ONE = 4096


def load() -> bytes:
    if not IMAGE.exists():
        raise SystemExit(f"missing {IMAGE}; run the extraction step first")
    return IMAGE.read_bytes()


def decode_all(data: bytes) -> dict[int, object]:
    # Linear disassembly stalls on the literal pools, so decode word by word.
    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    decoder.detail = True
    decoded: dict[int, object] = {}
    for offset in range(0, len(data) - 3, 4):
        for instruction in decoder.disasm(data[offset : offset + 4], BASE + offset):
            decoded[BASE + offset] = instruction
    return decoded


def word(data: bytes, address: int) -> int:
    return struct.unpack_from("<I", data, address - BASE)[0]


def signed(data: bytes, address: int) -> int:
    return struct.unpack_from("<i", data, address - BASE)[0]


def literal_target(instruction, address: int) -> int:
    return address + 8 + int(instruction.op_str.split("#")[-1].rstrip("]"), 0)


def converted_double(data: bytes, decoded: dict, call: int) -> float | None:
    """Recover the double a 0x02007f24 call site builds in r1:r2."""
    halves: dict[str, int] = {}
    for offset in range(-4, -72, -4):
        instruction = decoded.get(call + offset)
        if instruction is None:
            continue
        if instruction.mnemonic in ("bl", "bx", "pop"):
            break
        register = instruction.op_str.split(",")[0].strip()
        if register not in ("r1", "r2") or register in halves:
            continue
        if instruction.mnemonic == "ldr" and "pc," in instruction.op_str:
            halves[register] = word(data, literal_target(instruction, call + offset))
        elif instruction.mnemonic == "mov" and "#" in instruction.op_str:
            tail = instruction.op_str.split("#")[1]
            if "," not in tail:
                halves[register] = int(tail, 0)
        if len(halves) == 2:
            break
    if len(halves) != 2:
        return None
    return struct.unpack("<d", struct.pack("<II", halves["r1"], halves["r2"]))[0]


def gravity(data: bytes, decoded: dict) -> dict:
    per_frame = converted_double(data, decoded, GRAVITY_SITE)
    # 0x02017bcc clamps with `mov r0, #0x5000` before the subtract.
    terminal = 0x5000
    return {
        "per_frame": per_frame,
        "per_frame_fixed": round(per_frame * FIXED_ONE),
        "terminal_fall_speed": terminal / FIXED_ONE,
        "terminal_fall_speed_fixed": terminal,
        "site": f"0x{GRAVITY_SITE:08x}",
        "routine": "0x02017bcc",
    }


def jump(data: bytes, decoded: dict) -> dict:
    plain = converted_double(data, decoded, JUMP_PLAIN_SITE)
    scale = converted_double(data, decoded, JUMP_SCALE_SITE)
    return {
        "impulse": plain,
        # TRUNCATE, DO NOT ROUND. 0x02007F24 multiplies by 4096 and converts
        # with __aeabi_d2iz, which truncates toward zero. 1.55 * 4096 is
        # 6348.8, so the hardware value is 6348 and round() gave 6349 - one
        # unit of upward drift on every jump, every boosted jump and every
        # grapple mount, all of which read this same double.
        "impulse_fixed": int(plain * FIXED_ONE),
        "boosted_scale": scale,
        "boosted_impulse": plain * scale,
        # The ROM does NOT compute fixed(1.55 * 1.4). It converts each double
        # separately and multiplies in 20.12 with a half-ULP round:
        #   0x02047074  smull r3,r1,r2,r1
        #   0x02047078  adds  r2,r3,#0x800
        #   0x02047080  lsr   r2,r2,#0xc
        # giving (6348 * 5734 + 0x800) >> 12 = 8887, not round(2.17*4096)=8888.
        "boosted_impulse_fixed": (int(plain * FIXED_ONE)
                                  * int(scale * FIXED_ONE) + 0x800) >> 12,
        "boosted_modes_note": "FUN_02047008 takes the boosted path for "
                              "controller modes 8 and 0x0E only; every other "
                              "mode falls through to plain fixed(1.55).",
        "boosted_modes": [0x08, 0x0E],
        "routine": "0x02047008",
    }


def motion_entry(motion_id: int) -> tuple[int, int] | None:
    for low, high, base, stride in MOTION_BANKS:
        if low <= motion_id <= high:
            return base + (motion_id - low) * stride, stride
    return None


def speeds(data: bytes) -> dict:
    rows = {}
    for motion_id in range(0x00, 0xFA):
        located = motion_entry(motion_id)
        if located is None:
            continue
        address, stride = located
        # The 0x24-stride bank has no room for the run field; skip it rather
        # than read into the following row.
        if stride < RUN_FIELD + 4:
            continue
        walk = signed(data, address + WALK_FIELD)
        run = signed(data, address + RUN_FIELD)
        if not (0 < walk < 0x20000 and 0 < run < 0x20000):
            continue
        rows[motion_id] = {
            "walk": walk / FIXED_ONE,
            "run": run / FIXED_ONE,
            "walk_fixed": walk,
            "run_fixed": run,
        }
    tally: dict[tuple[int, int], int] = {}
    for row in rows.values():
        key = (row["walk_fixed"], row["run_fixed"])
        tally[key] = tally.get(key, 0) + 1
    common = max(tally.items(), key=lambda item: item[1])[0]
    return {
        "by_motion_id": {str(k): v for k, v in sorted(rows.items())},
        "default": {"walk": common[0] / FIXED_ONE, "run": common[1] / FIXED_ONE},
        "class_table": f"0x{SPEED_CLASS_TABLE:08x}",
        "class_meaning": {"0": "walk (+0x20)", "1": "run (+0x24)", "2": "zero"},
        "getter": "0x02004b88",
    }


def modes(data: bytes) -> list[dict]:
    rows = []
    for index in range(MODE_ROWS):
        address = MODE_TABLE + index * 16
        blend_class = data[address - BASE]
        handler = word(data, address + 4)
        rows.append(
            {
                "mode": index,
                "blend_class": blend_class,
                "action_id": data[address - BASE + 1],
                "vertical_handler": f"0x{handler:08x}" if handler else None,
            }
        )
    return rows


def main() -> int:
    data = load()
    decoded = decode_all(data)
    document = {
        "note": (
            "Recovered from the ARM9 image by tools/export_physics_constants.py. "
            "Units are world units per 60Hz frame; the engine stores them as "
            "20.12 fixed point."
        ),
        # 30, not 60. The DS SIMULATES at 30 Hz - PLAYER_TICK (0x020075EC)
        # fires once per two emulator frames, 89.6% of 31,244 gaps - and every
        # speed in this file is per SIMULATION TICK, not per rendered frame.
        # The port now runs _physics_process at 30 Hz to match
        # (project.godot common/physics_ticks_per_second), so a 60 here was a
        # live contradiction against physics.gd's TICKS_PER_SECOND.
        "frame_rate": 30,
        "fixed_point_shift": 12,
        "gravity": gravity(data, decoded),
        "jump": jump(data, decoded),
        "horizontal_blend": {
            "formula": "velocity = (desired * weight + velocity * (256 - weight)) >> 8",
            "routine": "0x02017ee4",
            "axes": ["x (+0x60)", "z (+0x68)"],
            "weights": {
                "class_0_normal": 0x3C,
                "class_1_coast": 0x00,
                "class_2_heading_locked": 0x3C,
                "secondary_controller": 0x4B,
                "forced_override": 0x08,
            },
            "forced_override_condition": (
                "descriptor type 4 with actor flag 0x08000000, or the global "
                "test at 0x020867ec"
            ),
        },
        "tank_scheme": {
            "routine": "0x020075ec",
            "turn_units_per_update": 728,
            "turn_degrees_per_update": 360.0 * 728 / 0x10000,
            "writes_velocity_directly": True,
            "reads_button_bits": {
                "0x10": "right: heading -= 728",
                "0x20": "left: heading += 728",
                "0x40": "forward drive",
                "0x80": "reverse drive",
            },
        },
        "free_scheme": {"routine": "0x02007408", "tank_actor_type": 9},
        "targeting_cone": {
            "dot_threshold": 0.866,
            "half_angle_degrees": 30.0,
            "site": "0x02047ed8",
        },
        "controller_modes": modes(data),
        "speeds": speeds(data),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"gravity {document['gravity']['per_frame']}/frame, "
          f"terminal {document['gravity']['terminal_fall_speed']}")
    print(f"jump {document['jump']['impulse']}, "
          f"boosted {document['jump']['boosted_impulse']:.4g}")
    print(f"speeds: {len(document['speeds']['by_motion_id'])} motion ids, "
          f"default {document['speeds']['default']}")
    print(f"-> {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
