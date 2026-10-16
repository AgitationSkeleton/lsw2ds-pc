"""Dump the netgame's OUTGOING shared block, so a change in it can be seen.

WHAT THIS IS FOR, in plain english. It was measured that holding a direction
during a match moves nobody, and a control run proved the harness CAN move
a character - it walks around the cantina quite happily in state 48. So the
freeze belongs to state 42, and the question becomes where the input stops.

In the cartridge's own multiplayer each console publishes a block of its own
state every frame and reads the other console's back. If the local input never
reaches that block, nothing the player does can affect anything, and BOTH
characters would stand still - which is exactly what was seen. This dumps
the block so "does holding a button change what this console publishes" is a
reading rather than an argument.

THE LAYOUT, from viewmask.h and the netgame analysis, resolved off the netgame object
rather than hardcoded, because the two consoles allocate it at different
addresses (host 0x021E08C8, child 0x0224391C):

    netgame + 0x40   the LOCAL block, published out to the other station
    netgame + 0x240  the INCOMING blocks, per station
    netgame + 0x640  where the outgoing one is built
    netgame + 0x6A0  the checksum        netgame + 0x6A8  the sequence

The netgame pointer itself lives at 0x021092F4.

    python tools/netblock.py              both consoles, one dump
    python tools/netblock.py --diff 5     dump, wait 5s, dump again, and say
                                          WHICH BYTES CHANGED

CAVEAT (e): an unreadable address and a zero read the same over this socket, so
a null netgame pointer is reported as such rather than printed as zeros.
"""

import json
import socket
import sys
import time

VIEW_HOLDER = 0x021092F4
OUT_BLOCK = 0x40                 # the local block published out
BUILD_BLOCK = 0x640              # where it is assembled
DUMP_LEN = 64                    # enough to see an input word move


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


def netgame(port):
    b = rd(port, VIEW_HOLDER, 4)
    if b is None or len(b) < 4:
        return None
    v = int.from_bytes(b, "little")
    return v or None


def grab(port):
    """Returns (netgame, out_bytes, build_bytes) or None."""
    ng = netgame(port)
    if ng is None:
        return None
    return (ng, rd(port, ng + OUT_BLOCK, DUMP_LEN),
            rd(port, ng + BUILD_BLOCK, DUMP_LEN))


def show(label, snap):
    if snap is None:
        print("%-5s  netgame pointer is NULL or unreadable" % label)
        return
    ng, out, bld = snap
    print("%-5s  netgame=%08X" % (label, ng))
    for name, off, data in (("out  +0x40", OUT_BLOCK, out),
                            ("build+0x640", BUILD_BLOCK, bld)):
        if data is None:
            print("        %s  unreadable" % name)
            continue
        for row in range(0, len(data), 16):
            print("        %s +%02X  %s" %
                  (name, row, data[row:row + 16].hex()))


def diff(label, a, b):
    if a is None or b is None:
        print("%-5s  cannot diff - a snapshot was missing" % label)
        return
    for name, x, y in (("out  +0x40", a[1], b[1]),
                       ("build+0x640", a[2], b[2])):
        if x is None or y is None:
            print("%-5s  %s  unreadable" % (label, name))
            continue
        changed = [i for i in range(min(len(x), len(y))) if x[i] != y[i]]
        if not changed:
            print("%-5s  %s  NO BYTE CHANGED" % (label, name))
        else:
            print("%-5s  %s  %d byte(s) changed at %s"
                  % (label, name, len(changed),
                     ",".join("+%02X" % c for c in changed[:16])))
            for c in changed[:8]:
                print("          +%02X  %02X -> %02X" % (c, x[c], y[c]))


def main():
    gap = 0.0
    if len(sys.argv) > 2 and sys.argv[1] == "--diff":
        gap = float(sys.argv[2])

    ports = ((19842, "HOST"), (19843, "CHILD"))
    first = {p: grab(p) for p, _ in ports}

    if gap <= 0:
        for p, label in ports:
            show(label, first[p])
        return 0

    time.sleep(gap)
    second = {p: grab(p) for p, _ in ports}
    print("what changed over %.1f s:" % gap)
    for p, label in ports:
        diff(label, first[p], second[p])
    return 0


if __name__ == "__main__":
    sys.exit(main())
