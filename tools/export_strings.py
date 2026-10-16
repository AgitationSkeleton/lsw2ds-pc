"""Decode the game's Huffman string bank - every UI string, by id.

WHY THIS MATTERS. The standing rule is that anything the port shows which is not
one of the game's own strings is an ADDITION, not parity. Until now only the
handful of strings a write-watchpoint happened to catch were known, so that rule
could not actually be checked. This decodes the bank.

THE DECODER is at 0x0205CD20 (its push; 0x0205CD70 is mid-loop). Its control
block is the literal at 0x0205CE18 = 0x020FE5C8, and the block is filled at load
- it reads zero in the static image, so this works from rebuilt RAM:

    [0x020FE5C8 + 0]   the OFFSET TABLE, indexed by string id * 4
    [0x020FE5C8 + 4]   the compressed DATA base
    [0x020FE5C8 + 8]   the HUFFMAN TREE base

THE TREE is u16 pairs. From the loop:

    0x0205CD74  add    r7, r6, sb, lsl #2
    0x0205CD78  subne  r7, r7, #0x300        bit set
    0x0205CD7C  ldrhne sb, [r7, #-0xfe]      -> tree + node*4 - 0x3FE
    0x0205CD80  subeq  r7, r7, #0x400        bit clear
    0x0205CD88  ldrheq sb, [r7]              -> tree + node*4 - 0x400

so node n has its two children at tree + (n - 0x100)*4 and +2, the root is
0x100, and any value below 0x100 is a literal byte. Bits are taken LOW FIRST -
`asr r7, r3, r0` then `tst r7, #1` with r0 counting 0..7 - and a fresh byte is
fetched when r0 reaches 8 (0x0205CD8C `cmp r0,#8` / `ldrbeq r3,[r4],#1`).
A decoded byte of 0 ends the string.

IT SELF-VALIDATES, which is why it can be trusted without a second source.
Character names are string 0x1CB + character id, and decoding those reproduces
config/menu_screens.json's independently-captured list exactly - 0x1DF "Greedo",
0x1D2 "Boba Fett", 0x1E8 "Jango Fett", 0x1ED "Lando Jabba's Palace". Strings
0x293 "@1 - @2" and 0x294 "????" likewise match what
character_select_overlay.gd already recorded from a framebuffer capture.

Usage:  python tools/export_strings.py
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import reconstruct_ram as rr  # noqa: E402

CAPTURE = ROOT / "out" / "work" / "session_live_01" / "inst0"
FRAME = 20000

CONTROL_BLOCK = 0x020FE5C8
ROOT_NODE = 0x100

# Character names are 0x1CB + character id. Used as the self-check.
NAME_BASE = 0x1CB
KNOWN_NAMES = {
    20: "Greedo", 0: "4-LOM", 7: "Boba Fett", 8: "Bossk", 13: "Dengar",
    27: "IG-88", 34: "Lando Jabba's Palace", 36: "Leia Bounty Hunter",
    71: "Zuckuss", 29: "Jango Fett",
}

# Strings recorded elsewhere in the repo, used as further checks.
KNOWN_STRINGS = {
    0x28E: "Scores",
    0x293: "@1 - @2",
    0x294: "????",
}

OUT = ROOT / "config" / "strings.json"


def main() -> int:
    ram, frame = rr.rebuild(CAPTURE, FRAME)
    base = rr.MAIN_RAM_BASE

    def u16(a):
        return struct.unpack_from("<H", ram, a - base)[0]

    def u32(a):
        return struct.unpack_from("<I", ram, a - base)[0]

    offsets = u32(CONTROL_BLOCK)
    data = u32(CONTROL_BLOCK + 4)
    tree = u32(CONTROL_BLOCK + 8)
    for name, value in (("offset table", offsets), ("data", data), ("tree", tree)):
        if not (0x02000000 <= value < 0x02400000):
            raise SystemExit("the %s pointer reads 0x%08X, which is not RAM - the "
                             "control block is filled at load, so a static read "
                             "will not do" % (name, value))

    def decode(sid: int, limit: int = 512) -> str:
        pointer = data + u32(offsets + sid * 4)
        current = ram[pointer - base]
        pointer += 1
        bit = 0
        out = bytearray()
        while len(out) < limit:
            node = ROOT_NODE
            while node >= ROOT_NODE:
                if (current >> bit) & 1:
                    node = u16(tree + node * 4 - 0x3FE)
                else:
                    node = u16(tree + node * 4 - 0x400)
                bit += 1
                if bit == 8:
                    current = ram[pointer - base]
                    pointer += 1
                    bit = 0
            byte = node & 0xFF
            if byte == 0:
                break
            out.append(byte)
        return out.decode("latin1")

    # --- the self-check, before anything is written ----------------------
    for cid, expected in sorted(KNOWN_NAMES.items()):
        got = decode(NAME_BASE + cid)
        if got != expected:
            raise SystemExit("string 0x%03X should be %r, decoded %r - the tree "
                             "walk is wrong" % (NAME_BASE + cid, expected, got))
    for sid, expected in sorted(KNOWN_STRINGS.items()):
        got = decode(sid)
        if got != expected:
            raise SystemExit("string 0x%03X should be %r, decoded %r"
                             % (sid, expected, got))

    # --- how many are there ----------------------------------------------
    #
    # The offset table is walked until an entry stops being a plausible,
    # non-decreasing offset into the data. Reading past the end is the failure
    # this project has hit repeatedly, so the bound is derived rather than
    # assumed.
    count = 0
    previous = -1
    while count < 4096:
        value = u32(offsets + count * 4)
        if value < previous or value > 0x00200000:
            break
        previous = value
        count += 1

    strings = {}
    printable = 0
    for sid in range(count):
        try:
            text = decode(sid)
        except Exception:  # noqa: BLE001
            break
        strings["0x%03X" % sid] = text
        if text and all(32 <= ord(c) < 127 for c in text):
            printable += 1

    document = {
        "schema": 1,
        "source": {
            "decoder": "0x0205CD20 (push); the loop is 0x0205CD5C..0x0205CE0C",
            "control_block": "0x%08X" % CONTROL_BLOCK,
            "offset_table": "0x%08X" % offsets,
            "data": "0x%08X" % data,
            "tree": "0x%08X" % tree,
            "capture": CAPTURE.parent.name,
            "frame": frame,
            "tree_layout": "u16 pairs at tree + (node-0x100)*4 and +2; root 0x100; "
                           "any value below 0x100 is a literal byte",
            "bit_order": "low bit first, fresh byte every 8 (0x0205CD8C)",
            "name_base": "character names are 0x%03X + character id" % NAME_BASE,
        },
        "note": __doc__.strip(),
        "count": count,
        "strings": strings,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1, ensure_ascii=False),
                   encoding="utf-8")

    print("control block 0x%08X -> offsets 0x%08X  data 0x%08X  tree 0x%08X"
          % (CONTROL_BLOCK, offsets, data, tree))
    print("self-check    %d character names and %d known strings reproduce exactly"
          % (len(KNOWN_NAMES), len(KNOWN_STRINGS)))
    print("decoded       %d strings, %d fully printable" % (count, printable))
    print()
    for sid in (0x28E, 0x28F, 0x290, 0x291, 0x292, 0x293, 0x294):
        print("  0x%03X  %r" % (sid, strings.get("0x%03X" % sid)))
    print()
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
