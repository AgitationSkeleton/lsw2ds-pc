"""Read "which player am I" on both consoles, so it can be compared across states.

WHAT THIS IS FOR, in plain english. It was established that in the cantina
lobby the local pad drives netgame SLOT 0 - holding a direction moves that
slot's position, and both consoles agree on where it ends up. In the match the
slots exist and neither ever moves. Both slots being frozen is equally
consistent with two very different faults:

    the pad drives NOTHING                     -> input never reaches a player
    the pad drives a slot that is not rendered -> input reaches the WRONG player

Those need different fixes, so this reads every value the game derives "which
player" from, in whatever state the consoles happen to be in, and prints them
side by side. Run it in the lobby and again in the match and compare.

THE DERIVATION, from viewmask.h and the view-manager analysis - the game computes every player
count from ONE bitmask, so a port write to any of the derived bytes is erased
the next time it recomputes:

    [0x021092F4]              the view manager
      + 0x240 + n*0x70        view record n
      + 0x66                  THE MASK - one bit per seated player
    [0x02104D2C + 2]          the mask, cached by the derivation at 0x02071B9C
    [0x02104D2C + 6]          the player COUNT, popcount of that mask
    [0x021027F8 + 0x29]       the same count, written four instructions later

    python tools/playerindex.py

CAVEAT (e): a null view manager is reported as such rather than printed as
zeros, because an unreadable address and a zero read identically here.
CAVEAT (t): the mask is a bitmask, not an index - bit n set means slot n is
seated. It says who EXISTS, not which one this console drives, and nothing here
should be read as the latter without saying so.
"""

import json
import socket
import sys

VIEW_HOLDER = 0x021092F4
VIEW_ARRAY = 0x240
VIEW_STRIDE = 0x70
VIEW_MASK = 0x66
DERIVED = 0x02104D2C          # +2 cached mask, +6 popcount
NETCOUNT = 0x021027F8         # +0x29 the same count again
STATE_MANAGER = 0x02102900


def rd(port, addr, ln):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    except OSError:
        return None
    try:
        f = s.makefile("rwb")
        f.write((json.dumps({"cmd": "read_mem", "cpu": 9,
                             "addr": addr, "len": ln}) + "\n").encode())
        f.flush()
        line = f.readline().decode()
    finally:
        s.close()
    try:
        h = json.loads(line).get("hex", "")
    except Exception:
        return None
    return bytes.fromhex(h) if h else None


def u32(b):
    return None if (b is None or len(b) < 4) else int.from_bytes(b[:4], "little")


def u16(b):
    return None if (b is None or len(b) < 2) else int.from_bytes(b[:2], "little")


def u8(b):
    return None if (b is None or len(b) < 1) else b[0]


def bits(mask):
    if mask is None:
        return "?"
    return ",".join(str(i) for i in range(4) if mask & (1 << i)) or "none"


def report(port, label):
    obj = u32(rd(port, STATE_MANAGER + 4, 4))
    state = u32(rd(port, obj + 4, 4)) if obj else None
    vm = u32(rd(port, VIEW_HOLDER, 4))

    print("%-5s  state=%-4s view manager=%s" %
          (label, state,
           "NULL" if not vm else "%08X" % vm))
    if not vm:
        print("        no view manager - the rest would be meaningless")
    else:
        for n in range(4):
            m = u16(rd(port, vm + VIEW_ARRAY + n * VIEW_STRIDE + VIEW_MASK, 2))
            print("        record %d mask[+0x66] = %s   seated slots: %s"
                  % (n, "?" if m is None else "0x%04X" % m, bits(m)))

    cached = u16(rd(port, DERIVED + 2, 2))
    count = u8(rd(port, DERIVED + 6, 1))
    netc = u8(rd(port, NETCOUNT + 0x29, 1))
    print("        cached mask [02104D2E] = %s   seated: %s"
          % ("?" if cached is None else "0x%04X" % cached, bits(cached)))
    print("        player count [02104D32] = %s      [02102821] = %s"
          % (count, netc))


def main():
    ports = ((19842, "HOST"), (19843, "CHILD"))
    if len(sys.argv) > 1:
        ports = ((int(sys.argv[1]), "PORT%s" % sys.argv[1]),)
    for p, label in ports:
        report(p, label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
