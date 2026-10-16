"""Does the arena player respond to the d-pad?

THE GOAL THIS SERVES, IN PLAIN ENGLISH. The goal is two connected players
in a real arena deathmatch, not warped onto the map as statues. An earlier
investigation found that the arena's behaviour value 9 is not a broken value at all: it selects
`FUN_020075ec`, a TANK-CONTROL movement handler that reads a button word at
`[actor+0x5CE]` and treats

    0x10 RIGHT / 0x20 LEFT  -> turn on the spot, changing the heading at +0xCA
    0x40 UP    / 0x80 DOWN  -> drive, changing the position at +0xB0

and that holding RIGHT on a live match really did set that word to 0x0010. So
the one thing left to know is whether UP actually moves the player. Pressing a
TURN key and watching the position not change - which is what every earlier test
did - is what tank controls are supposed to do.

WHY THIS IS A SCRIPT AND NOT THREE PILOT CALLS. Two instrument facts, both learned
the hard way:

  1. THE DEBUG SERVER SERIALIZES CONNECTIONS. A second socket opened to read
     memory while an input connection is held simply times out. Input and reads
     must share one connection, which `pilot.py` cannot do.

  2. A STALLED CONSOLE READS ZERO FOR EVERY KEY AND LOOKS EXACTLY LIKE "input
     does not arrive". That is precisely how an earlier UP trial went void: the
     host had stopped responding, read 0 for RIGHT - a key that had just read
     0x0010 - and only a `Responding = False` check afterwards showed why. So
     every trial here is bracketed by a LIVENESS CHECK, and a trial whose
     console did not step is reported as VOID rather than as a zero.

A refused or stale read is not a zero. That is the whole reason for the guard.
"""

from __future__ import annotations

import json
import socket
import sys
import time

HOST = "127.0.0.1"

# Read live from the guest rather than hardcoded: the actor array is at
# 0x0210930C + 0x4C, and actor addresses differ per level (caveat (bb)).
ACTOR_ARRAY = 0x02109358
BEHAVIOUR_TABLE = 0x02109E90

# The input manager, from FUN_0205d140(mgr, slot):
#     if slot == 4: slot = [mgr+0x2A]        ; "me"
#     return [mgr + slot*10]                 ; buttons, halfword
# so it is four 10-byte per-slot records with the local player index parked at
# +0x2A, just past them. Reading it separates "the pad never reached the input
# manager" from "the manager has the buttons but the actor does not" - and in a
# networked match the remote player's slot is fed over the wire, so this also
# shows whether the other console's input is arriving at all.
INPUT_MGR = 0x021027F8
MGR_STRIDE = 10
MGR_LOCAL_INDEX = 0x2A

OFF_POS = 0xB0      # x,y,z, three words
OFF_HEADING = 0xCA  # halfword
OFF_CTRL = 0x5CE    # the button word
OFF_PREV = 0x5D0
OFF_SLOT = 0x5DA


class Link:
    """One connection, used for BOTH input and reads - see fact (1) above."""

    def __init__(self, port: int, timeout: float = 8.0):
        self.sock = socket.create_connection((HOST, port), timeout=timeout)
        self.f = self.sock.makefile("rwb")

    def cmd(self, **kw):
        self.f.write((json.dumps(kw) + "\n").encode())
        self.f.flush()
        return json.loads(self.f.readline().decode())

    def read(self, addr: int, ln: int) -> bytes:
        r = self.cmd(cmd="read_mem", cpu=9, addr=addr, len=ln)
        return bytes.fromhex(r.get("hex", ""))

    def u16(self, addr: int) -> int:
        b = self.read(addr, 2)
        return int.from_bytes(b, "little") if len(b) == 2 else -1

    def pos(self, actor: int):
        b = self.read(actor + OFF_POS, 12)
        if len(b) != 12:
            return None
        return tuple(int.from_bytes(b[i:i + 4], "little", signed=True)
                     for i in (0, 4, 8))

    def key(self, name: str, down: bool):
        return self.cmd(cmd="frontend_input", action="key", key=name, down=down)

    def frames(self) -> int:
        """A monotonically advancing counter, for the liveness guard.

        NOT `frontend_stats.frames`. That key exists and reads 0 forever on a
        perfectly healthy console, which made the guard declare every trial
        void - the same failure mode, in the instrument this time, that the
        guard exists to catch. `dispatch_stats.arm9.dispatch_total` is the
        recompiler's own dispatch count and advances whenever the guest runs.
        """
        try:
            st = self.cmd(cmd="dispatch_stats")
            return int(st["arm9"]["dispatch_total"])
        except Exception:                                    # noqa: BLE001
            return -1

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def stepping(link: Link, actor: int, dwell: float = 0.6) -> bool:
    """Is this console actually running frames right now?

    Prefers a frame counter; falls back to watching any of several live fields
    change, because a console can be stepping while the player stands still.
    """
    a = link.frames()
    if a >= 0:
        time.sleep(dwell)
        return link.frames() > a
    # No counter exposed - fall back on the animation timer next to the pose.
    probe = actor + 0xD0
    before = link.read(probe, 16)
    time.sleep(dwell)
    return link.read(probe, 16) != before


def trial(link: Link, actor: int, key: str, hold_s: float = 1.6):
    """Hold one key and report what moved, or VOID if the console stalled."""
    if not stepping(link, actor):
        return {"key": key, "void": "console not stepping BEFORE the trial"}

    p0, h0 = link.pos(actor), link.u16(actor + OFF_HEADING)
    link.key(key, True)
    seen_ctrl = set()
    seen_mgr = set()
    t_end = time.time() + hold_s
    while time.time() < t_end:
        time.sleep(0.12)
        seen_ctrl.add(link.u16(actor + OFF_CTRL))
        seen_mgr.add(tuple(link.u16(INPUT_MGR + s * MGR_STRIDE) for s in range(4)))
    p1, h1 = link.pos(actor), link.u16(actor + OFF_HEADING)
    link.key(key, False)
    time.sleep(0.25)

    if not stepping(link, actor):
        return {"key": key, "void": "console stopped stepping DURING the trial"}

    return {
        "key": key,
        "ctrl": sorted(v for v in seen_ctrl if v >= 0),
        "mgr": sorted(seen_mgr),
        "pos_before": p0, "pos_after": p1, "pos_moved": p0 != p1 and None not in (p0, p1),
        "heading_before": h0, "heading_after": h1, "heading_moved": h0 != h1 and -1 not in (h0, h1),
    }


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19842
    slot = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    trials = int(sys.argv[3]) if len(sys.argv) > 3 else 3

    link = Link(port)
    try:
        arr = link.read(ACTOR_ARRAY, 16)
        actors = [int.from_bytes(arr[i * 4:i * 4 + 4], "little") for i in range(4)]
        actor = actors[slot]
        print("port %d  slot %d  actor %08X  (array: %s)"
              % (port, slot, actor, " ".join("%08X" % a for a in actors)))
        if not (0x02000000 <= actor < 0x02400000):
            print("  actor address is not in RAM - not in a level yet; nothing to test")
            return 2
        print("  actor +0x5DA (its own slot byte) = %d   <- must equal %d"
              % (link.read(actor + OFF_SLOT, 1)[0], slot))
        print("  behaviour table = %s"
              % link.read(BEHAVIOUR_TABLE, 4).hex())
        print("  NOTE: 0x10/0x20 are TURN keys - they are not supposed to move you.")
        print()

        for t in range(1, trials + 1):
            print("---- trial %d ----" % t)
            for key in ("up", "down", "left", "right"):
                r = trial(link, actor, key)
                if "void" in r:
                    print("  %-5s VOID: %s" % (key.upper(), r["void"]))
                    continue
                mgr = "/".join("[" + " ".join("%04X" % v for v in m) + "]"
                               for m in r["mgr"])
                print("  %-5s ctrl=%s  mgr=%s\n        pos %s -> %s  moved=%s | heading %04X -> %04X  turned=%s"
                      % (key.upper(),
                         "/".join("%04X" % c for c in r["ctrl"]) or "-",
                         mgr or "-",
                         r["pos_before"], r["pos_after"], r["pos_moved"],
                         r["heading_before"] & 0xFFFF, r["heading_after"] & 0xFFFF,
                         r["heading_moved"]))
    finally:
        for key in ("up", "down", "left", "right"):
            try:
                link.key(key, False)
            except Exception:                                # noqa: BLE001
                pass
        link.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
