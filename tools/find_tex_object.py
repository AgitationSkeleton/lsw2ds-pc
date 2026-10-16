"""Find the game's texture OBJECT behind a polygon's texture offset.

WHY. The top-screen portrait was traced to `FUN_0205a148`, which builds
TEXIMAGE_PARAM out of a small object:

    +0x09  enable flag, zero means "draw nothing"
    +0x0C  texture format   (3 = 16-colour)
    +0x10  VRAM offset      (its low 16 bits ARE the parameter's low 16 bits)

The portrait's face polygon carries texture offset 0x03DE60, so the object
behind it has `(+0x10 & 0xffff) == 0x7BCC`. Finding that object is what turns
"the icon never changes" into an address that can be watched or poked.

A plain 32-bit scan for 0x00007BCC finds nothing, because the field's upper half
is not necessarily zero. This scans on the low half only, and then CHECKS THE
STRUCT SHAPE around each hit - format and enable byte - so a coincidental
halfword in unrelated data is not reported as a texture object.

Usage:
    python tools/find_tex_object.py 0x7BCC [--format 3] [--port 19842]
    python tools/find_tex_object.py 0x7BCC 0x7ACC        (face and ring)
"""

import json
import socket
import sys

START = 0x02000000
END = 0x02400000
CHUNK = 0x10000


def rd(port, addr, ln):
    s = socket.create_connection(("127.0.0.1", port), timeout=30.0)
    f = s.makefile("rwb")
    f.write((json.dumps({"cmd": "read_mem", "cpu": 9, "addr": addr, "len": ln})
             + chr(10)).encode())
    f.flush()
    h = json.loads(f.readline().decode()).get("hex", "")
    s.close()
    return bytes.fromhex(h) if h else b""


def main():
    wants = []
    fmt = 3
    port = 19842
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--format":
            fmt = int(args[i + 1], 0)
            i += 2
        elif args[i] == "--port":
            port = int(args[i + 1], 0)
            i += 2
        else:
            wants.append(int(args[i], 0) & 0xFFFF)
            i += 1
    if not wants:
        print(__doc__)
        return

    print("looking for texture objects whose +0x10 low half is one of %s"
          % ", ".join("%04X" % w for w in wants))

    # The whole of main RAM is pulled once and searched locally. Reading in
    # 64KB pieces keeps each debug-server reply to a sane JSON line, and doing
    # the matching here rather than per-candidate avoids a second pass over the
    # socket for every hit.
    blob = bytearray()
    addr = START
    short = 0
    while addr < END:
        n = min(CHUNK, END - addr)
        try:
            d = rd(port, addr, n)
        except Exception as e:
            print("  read failed at %08X: %s" % (addr, e))
            d = b""
        if len(d) != n:
            short += 1
            d = d + bytes(n - len(d))
        blob += d
        addr += n
    if short:
        print("  NOTE: %d of %d chunks came back short and were zero-filled - "
              "a miss in those regions is not evidence."
              % (short, (END - START) // CHUNK))

    found = 0
    # +0x10 is word aligned inside a word-aligned object, so only word offsets
    # need testing.
    for off in range(0, len(blob) - 4, 4):
        lo = blob[off] | (blob[off + 1] << 8)
        if lo not in wants:
            continue
        obj = off - 0x10          # the object starts 0x10 before the field
        if obj < 0:
            continue
        # THE SHAPE CHECK. Without it this prints hundreds of coincidences.
        f = int.from_bytes(blob[obj + 0x0C:obj + 0x10], "little")
        en = blob[obj + 9]
        if f != fmt or en == 0:
            continue
        found += 1
        print("  object %08X   +09 enable %02X  +0C format %d  +10 %08X"
              % (START + obj, en, f,
                 int.from_bytes(blob[off:off + 4], "little")))
        if found >= 40:
            print("  ... 40 shown; this is the print limit, not the total.")
            break
    if found == 0:
        print("  none matched. The halfword may be present without the struct")
        print("  shape around it, which would mean the object is not laid out")
        print("  the way FUN_0205a148 reads it - not that the portrait is absent.")


main()
