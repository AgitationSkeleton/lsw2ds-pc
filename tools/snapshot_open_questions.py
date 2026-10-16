"""Read, in one shot, every value the open questions are waiting on.

WHY THIS EXISTS. Static analysis has taken a number of features as far as it
can, and each one ended at the same kind of sentence: "that is a runtime
question, and a short one". There are now nine of those spread across nine
write-ups. Nine separate runs is a test nobody wants to do; one command run
three times is not.

    python tools/snapshot_open_questions.py --label "fresh save, in world"
    python tools/snapshot_open_questions.py --label "unlocked save, in world"
    python tools/snapshot_open_questions.py --label "arena lobby"

Each run appends a labelled block to out/work/open_questions.txt and prints it.
Diffing two blocks is what answers most of the questions - which array holds the
unlock state, in particular, is settled entirely by which flag array differs
between the first two.

WHAT IT DOES NOT DO. It does not drive the game. The port must already be
running and at the point the label describes; this only reads. That is
deliberate - the game window is not to be taken over while it runs, and a tool
that reads through the debug surface never touches it.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out" / "work" / "open_questions.txt"

HOST, PORT = "127.0.0.1", 19842

SAVE_BLOCK = 0x02104D90

# The hand-read table. Nine arrays, four words each.
FLAG_ARRAYS = [
    (0x4488, "mode bitfield: bit 10 Free Play, bit 11 multi-view"),
    (0x44CC, "earlier analysis calls this the unlock test - DISPUTED"),
    (0x44DC, "the other half of the pair over character/vehicle ids"),
    (0x44EC, ""),
    (0x450C, ""),
    (0x4514, "range from 22; a shop's purchased flags is the lead"),
    (0x4518, "EXTRAS, corroborated four ways"),
    (0x451C, ""),
    (0x4520, ""),
]

# Five customization part slots, 0xFA meaning "none".
CUSTOMIZATION = 0x02107890
CUSTOMIZATION_SLOTS = [0xA6, 0xA8, 0xAA, 0xAC, 0xAE]

# The arena settings object, reached through a pointer.
ARENA_SETTINGS_PTR = 0x02107860

# The multi-view flag and the camera array.
GAME_STATE_WORD = 0x027E02A8
CAMERA_ARRAY_PTR = 0x021092F4

# The WH context, meaningful only during a wireless session.
WH_CONTEXT = 0x0210B320

# The level, two ways.
STATE_MANAGER = 0x02102900
LEVEL_ROOT = 0x0210930C

# The music slot the game currently has playing.
MUSIC_CTX = 0x0210DB14


class Surface:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=30.0)
        self.buf = b""

    def cmd(self, **kw):
        self.sock.sendall((json.dumps(kw) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("the port closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def read(self, addr, n):
        return bytes.fromhex(self.cmd(cmd="read_mem", addr=addr, len=n)["hex"])

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u16(self, addr):
        return struct.unpack("<H", self.read(addr, 2))[0]

    def u8(self, addr):
        return self.read(addr, 1)[0]


def plausible(ptr):
    return 0x02000000 <= ptr < 0x02400000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True,
                    help='where this reading was taken, e.g. "fresh save, in world"')
    args = ap.parse_args()

    try:
        s = Surface()
    except OSError as exc:
        print("cannot reach the port on %s:%d - %s" % (HOST, PORT, exc),
              file=sys.stderr)
        print("Start the game and get it to the point you are labelling.",
              file=sys.stderr)
        return 1

    lines = []

    def out(fmt, *a):
        lines.append(fmt % a if a else fmt)

    out("=" * 72)
    out("LABEL: %s", args.label)
    out("=" * 72)

    # --- where are we -----------------------------------------------------
    ctx = s.u32(STATE_MANAGER)
    level = s.u32(ctx + 0x0C) if plausible(ctx) else -1
    out("")
    out("context 0x%08X   level index %s", ctx,
        level if level >= 0 else "not in a level")

    # --- unlocks: the nine flag arrays --------------------------------------
    out("")
    out("-- the nine save-block flag arrays -----------------------------------")
    out("   DIFF THESE between a fresh save and a fully-unlocked one. The array")
    out("   that differs is the unlock state, and unlocking everything follows.")
    for off, note in FLAG_ARRAYS:
        words = struct.unpack("<4I", s.read(SAVE_BLOCK + off, 16))
        out("   +0x%04X  %08X %08X %08X %08X  %s", off, *words, note)

    # --- customization -----------------------------------------------------
    out("")
    out("-- customization part slots ------------------------------------------")
    out("   0xFA means 'no part'. Five slots; whether that is the whole set is")
    out("   not established.")
    vals = [s.u16(CUSTOMIZATION + o) for o in CUSTOMIZATION_SLOTS]
    out("   0x%08X  %s", CUSTOMIZATION,
        "  ".join("+0x%02X=%04X" % (o, v) for o, v in zip(CUSTOMIZATION_SLOTS, vals)))

    # --- the arena scoreboard ----------------------------------------------
    out("")
    out("-- arena settings and scoreboard -------------------------------------")
    settings = s.u32(ARENA_SETTINGS_PTR)
    if plausible(settings):
        out("   object 0x%08X", settings)
        for off, what in ((0x14, "counter A"), (0x1C, "counter B"),
                          (0x24, "counter C")):
            slots = struct.unpack("<4h", s.read(settings + off, 8))
            out("     +0x%02X  %6d %6d %6d %6d   %s", off, *slots, what)
        order = s.read(settings + 0x30, 4)
        out("     +0x30  slot order %s", list(order))
        out("     +0x34  mode %d   +0x40  arena level %d",
            s.u8(settings + 0x34), s.u8(settings + 0x40))
    else:
        out("   pointer reads 0x%08X - not built here", settings)

    # --- views: the multi-view flag and camera -----------------------------
    out("")
    out("-- multi-view and the camera array -----------------------------------")
    state = s.u32(GAME_STATE_WORD)
    cam = s.u32(CAMERA_ARRAY_PTR)
    out("   [0x027E02A8] = %08X   bit 0x40 %s", state,
        "SET - per-index cameras exist" if state & 0x40 else "clear")
    out("   0x021092F4   = %08X   %s", cam,
        "allocated" if plausible(cam) else "null")

    # --- views: the party and their view indices --------------------------
    out("")
    out("-- party actors and their view index, actor+0x5DA --------------------")
    out("   4 means 'no view of my own' and forces world-absolute movement.")
    root = s.u32(LEVEL_ROOT)
    if plausible(root):
        for i in range(4):
            actor = s.u32(root + 0x4C + i * 4)
            if not plausible(actor):
                out("   slot %d  empty", i)
                continue
            out("   slot %d  actor 0x%08X  view index %d  character id %d",
                i, actor, s.u8(actor + 0x5DA), s.u8(actor + 0x6BB))
    else:
        out("   level root reads 0x%08X - no level", root)

    # --- the WH context ----------------------------------------------------
    out("")
    out("-- WH context, meaningful only in a wireless session -----------------")
    out("   +0x00 doubleMode %d   +0x0E %d   +0x20 state %d   +0x30 errcode %d",
        s.u8(WH_CONTEXT + 0x00), s.u16(WH_CONTEXT + 0x0E),
        s.u32(WH_CONTEXT + 0x20), s.u32(WH_CONTEXT + 0x30))
    out("   +0x44 connect mode %d   (4 = DataSharing parent, 5 = child)",
        s.u32(WH_CONTEXT + 0x44))

    # --- music -------------------------------------------------------------
    out("")
    out("-- music -------------------------------------------------------------")
    blob = s.read(MUSIC_CTX + 9, 2)
    out("   slot playing %d   flags %02X%s", blob[0], blob[1],
        "  [suppressed]" if blob[1] & 0x20 else "")

    text = "\n".join(lines) + "\n\n"
    print(text)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "a", encoding="utf-8") as fh:
        fh.write(text)
    print("appended to %s" % OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
