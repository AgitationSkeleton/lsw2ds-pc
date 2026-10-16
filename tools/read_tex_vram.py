"""Read the 3D engine's TEXTURE address space out of the running port.

WHY THIS EXISTS, AND WHY THE OBVIOUS THING IS WRONG. A polygon's TEXIMAGE_PARAM
names its texture by an offset into the geometry engine's own texture space -
0x03DE60 for the top-screen portrait's face. That is NOT a CPU address. Adding
it to 0x06000000 and reading gives 512 bytes of zeroes, which looks exactly like
a real measurement and is not one; it cost this project a run.

Texture space is four 128KB SLOTS, filled by whichever VRAM banks A-D have been
assigned to them through VRAMCNT with MST=3. The CPU can only see those banks at
their LCDC addresses. So resolving a texture address means reading VRAMCNT,
finding which bank owns the slot the address falls in, and reading that bank.

That is what this does, and it PRINTS THE MAPPING IT USED - because a wrong
mapping and an unwritten texture produce the same zeroes, and the whole point of
this file is to stop that confusion happening twice.

Usage:
    python tools/read_tex_vram.py <tex_offset> <length> [port]
    python tools/read_tex_vram.py 0x03DE60 512
    python tools/read_tex_vram.py --map            (just show the mapping)
"""

import hashlib
import json
import socket
import sys

# VRAM banks as the CPU sees them when a bank is mapped to LCDC, and their
# sizes. Only A-D can back texture space, but the whole table is here because
# the mapping dump is worth having complete.
BANKS = [
    ("A", 0x06800000, 128 * 1024),
    ("B", 0x06820000, 128 * 1024),
    ("C", 0x06840000, 128 * 1024),
    ("D", 0x06860000, 128 * 1024),
    ("E", 0x06880000, 64 * 1024),
    ("F", 0x06890000, 16 * 1024),
    ("G", 0x06894000, 16 * 1024),
    ("H", 0x06898000, 32 * 1024),
    ("I", 0x068A0000, 16 * 1024),
]

SLOT = 128 * 1024


def rd(port, addr, ln):
    """Read guest memory, in pieces small enough for one JSON line each."""
    out = b""
    while ln > 0:
        n = min(ln, 0x1000)
        s = socket.create_connection(("127.0.0.1", port), timeout=20.0)
        f = s.makefile("rwb")
        f.write(
            (json.dumps({"cmd": "read_mem", "cpu": 9, "addr": addr, "len": n})
             + chr(10)).encode()
        )
        f.flush()
        h = json.loads(f.readline().decode()).get("hex", "")
        s.close()
        if not h:
            raise RuntimeError("empty read at %08X" % addr)
        out += bytes.fromhex(h)
        addr += n
        ln -= n
    return out


def texture_slots(port):
    """Which bank backs each of the four texture slots, from VRAMCNT_A..D."""
    cnt = rd(port, 0x04000240, 4)
    slots = {}
    detail = []
    for i in range(4):
        v = cnt[i]
        name, base, size = BANKS[i]
        enabled = (v & 0x80) != 0
        mst = v & 7
        ofs = (v >> 3) & 3
        detail.append(
            "  VRAMCNT_%s = %02X  %s  MST=%d OFS=%d%s"
            % (name, v, "enabled" if enabled else "DISABLED", mst, ofs,
               "  <- texture slot %d" % ofs if (enabled and mst == 3) else "")
        )
        if enabled and mst == 3:
            slots[ofs] = (name, base, size)
    return slots, detail


def main():
    args = sys.argv[1:]
    port = 19842
    if "--map" in args:
        slots, detail = texture_slots(port)
        print("VRAM bank control:")
        for d in detail:
            print(d)
        print("texture slots backed: %s"
              % (", ".join("%d=%s" % (k, v[0]) for k, v in sorted(slots.items()))
                 or "NONE"))
        return

    if len(args) < 2:
        print(__doc__)
        return

    off = int(args[0], 0)
    ln = int(args[1], 0)
    if len(args) > 2:
        port = int(args[2], 0)

    slots, detail = texture_slots(port)
    for d in detail:
        print(d)

    slot = off // SLOT
    if slot not in slots:
        print("texture offset %06X falls in slot %d, and NO BANK IS MAPPED "
              "THERE." % (off, slot))
        print("Any read would be meaningless, so none was made.")
        return

    name, base, size = slots[slot]
    within = off % SLOT
    addr = base + within
    print("texture %06X -> slot %d -> bank %s -> CPU address %08X"
          % (off, slot, name, addr))

    data = rd(port, addr, ln)
    nz = sum(1 for b in data if b)
    print("%d bytes  sha1 %s  nonzero %d/%d"
          % (len(data), hashlib.sha1(data).hexdigest()[:16], nz, len(data)))
    print("first 32: %s" % data[:32].hex())
    if nz == 0:
        print("ALL ZERO - and the mapping above is real, so this is a genuinely")
        print("blank texture rather than a bad address.")


main()
