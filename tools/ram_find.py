"""Search the running game's memory for a byte pattern.

WHY THIS EXISTS. Plenty of things are known by their layout in a FILE and not by
their address in RAM - the save game is the obvious one. The save's structure was
worked out by diffing .sav files, but a cheat menu has to write to the copy the
game is actually reading, and nothing in the ROM says where that is.

The game itself answers this: load a save whose contents are known, then look for
those bytes in memory. Whatever matches is the mirror. That is a measurement, not
a guess, and it is repeatable - which matters because the address can move
between builds and this can simply be re-run.

The port must be running with its debug surface up (port/launch.sh --serve, or
lsw2.exe with --serve).

    # where did this file's bytes end up?
    python tools/ram_find.py --file port/saves/unlocked-template.sav --offset 0x60 --len 32

    # or an explicit pattern
    python tools/ram_find.py --hex "ffff f7bf ffdf 7dfd"

    # narrow to one region (default searches main RAM)
    python tools/ram_find.py --hex ... --region wramshared

Regions the surface exposes: mainram, wram7, wramshared, itcm, dtcm.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import sys

HOST, PORT = "127.0.0.1", 19842

# Where each region actually lives in the guest's address space, so a hit is
# reported as an address the game would use rather than an offset into a blob.
REGION_BASE = {
    "mainram": 0x02000000,
    "wramshared": 0x03000000,
    "wram7": 0x03800000,
    "itcm": 0x01FF8000,
    "dtcm": 0x027E0000,
}


class Surface:
    def __init__(self, host=HOST, port=PORT, timeout=30.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def command(self, **kw):
        self.sock.sendall((json.dumps(kw) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("the port closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())


def parse_hex(text: str) -> bytes:
    cleaned = "".join(c for c in text if c in "0123456789abcdefABCDEF")
    if len(cleaned) % 2:
        raise SystemExit("hex pattern has an odd number of digits")
    return bytes.fromhex(cleaned)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hex", help="pattern as hex, spaces ignored")
    ap.add_argument("--file", help="take the pattern from this file")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--len", type=lambda s: int(s, 0), default=32)
    ap.add_argument("--region", default="mainram")
    ap.add_argument("--max-hits", type=int, default=20)
    args = ap.parse_args()

    if args.hex:
        pattern = parse_hex(args.hex)
    elif args.file:
        data = pathlib.Path(args.file).read_bytes()
        pattern = data[args.offset:args.offset + args.len]
    else:
        raise SystemExit("give --hex or --file")

    if not pattern:
        raise SystemExit("empty pattern")
    # A pattern that is all one byte matches everywhere and tells you nothing.
    if len(set(pattern)) == 1:
        print("warning: every byte of the pattern is 0x%02x - expect noise"
              % pattern[0], file=sys.stderr)

    try:
        surface = Surface()
    except OSError as exc:
        print("cannot reach the port on %s:%d - %s" % (HOST, PORT, exc),
              file=sys.stderr)
        print("Start it with the debug surface enabled.", file=sys.stderr)
        return 1

    reply = surface.command(cmd="read_region", region=args.region)
    if "hex" not in reply:
        print("read_region failed: %s" % reply, file=sys.stderr)
        return 1
    blob = bytes.fromhex(reply["hex"])
    base = REGION_BASE.get(args.region, 0)

    print("pattern (%d bytes): %s" % (len(pattern), pattern.hex(" ")))
    print("searching %s, %d bytes, base %08X" % (args.region, len(blob), base))

    hits = []
    start = 0
    while True:
        i = blob.find(pattern, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
        if len(hits) >= args.max_hits:
            break

    if not hits:
        print("no match")
        return 2
    for i in hits:
        print("  %08X  (offset %06X)" % (base + i, i))
    if len(hits) >= args.max_hits:
        print("  ... stopped at %d hits" % args.max_hits)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
