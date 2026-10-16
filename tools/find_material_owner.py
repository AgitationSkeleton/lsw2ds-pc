"""Find the material object carrying a given TEXIMAGE_PARAM, then who owns it.

WHY. Earlier tracing followed the co-op marker arrow (visible in the lobby when it
should not be) to an ITCM material-bind routine that reads a material object:

    +0x04  diffuse/ambient
    +0x08  specular/emission
    +0x0C  polygon attribute
    +0x10  TEXIMAGE_PARAM     <- 0x0803682C for the marker
    +0x14  palette base

That routine is generic - it binds a material for anything that has one - so it
does not identify the arrow. The MATERIAL does. This finds it by its contents
and then walks up to whatever holds a pointer to it, which is the same
three-step route that took the top-screen portrait bug from pixels to a fix: find
the object by what the game wrote into it, find what holds it, read that holder's
flags.

ONE SNAPSHOT, MANY QUESTIONS. Main RAM is pulled once and every search runs
against that copy. Doing it per level over the socket was slow enough to matter,
and worse, it let the guest move between levels of the walk - which is exactly
the sort of thing that produces a chain that never existed.

Usage:
    python tools/find_material_owner.py 0x0803682C [--port 19843] [--depth 2]
"""

import json
import socket
import sys

START = 0x02000000
END = 0x02400000
CHUNK = 0x10000

# The material layout read at the bind. The texture parameter is the needle; the
# rest are printed so a candidate can be judged rather than assumed.
FIELDS = [(0x00, "flags?"), (0x04, "dif/amb"), (0x08, "spe/emi"),
          (0x0C, "polyattr"), (0x10, "teximage"), (0x14, "pltt")]


def rd(port, addr, ln):
    s = socket.create_connection(("127.0.0.1", port), timeout=30.0)
    f = s.makefile("rwb")
    f.write((json.dumps({"cmd": "read_mem", "cpu": 9, "addr": addr, "len": ln})
             + chr(10)).encode())
    f.flush()
    h = json.loads(f.readline().decode()).get("hex", "")
    s.close()
    return bytes.fromhex(h) if h else b""


def snapshot(port):
    blob = bytearray()
    addr = START
    short = 0
    while addr < END:
        n = min(CHUNK, END - addr)
        try:
            d = rd(port, addr, n)
        except Exception:
            d = b""
        if len(d) != n:
            short += 1
            d = d + bytes(n - len(d))
        blob += d
        addr += n
    if short:
        print("NOTE: %d chunks read short and were zero-filled; a miss inside "
              "them is not evidence." % short)
    return bytes(blob)


def w32(blob, addr):
    o = addr - START
    if o < 0 or o + 4 > len(blob):
        return None
    return int.from_bytes(blob[o:o + 4], "little")


def holders(blob, value):
    need = value.to_bytes(4, "little")
    out = []
    off = 0
    while True:
        off = blob.find(need, off)
        if off < 0:
            break
        if off % 4 == 0:
            out.append(START + off)
        off += 4
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    needle = int(sys.argv[1], 0) & 0xFFFFFFFF
    port = 19843
    depth = 2
    a = sys.argv[2:]
    i = 0
    while i < len(a):
        if a[i] == "--port":
            port = int(a[i + 1], 0); i += 2
        elif a[i] == "--depth":
            depth = int(a[i + 1], 0); i += 2
        else:
            i += 1

    blob = snapshot(port)
    hits = holders(blob, needle)
    print("TEXIMAGE_PARAM %08X appears at %d aligned word(s)"
          % (needle, len(hits)))
    if not hits:
        print("  none. Either the marker is not on screen in this state, or")
        print("  the material lives outside main RAM. Check the picture first.")
        return

    materials = []
    for h in hits:
        base = h - 0x10
        if base < START:
            continue
        row = []
        for off, name in FIELDS:
            v = w32(blob, base + off)
            row.append("%s=%08X" % (name, v if v is not None else 0))
        print("  material %08X  %s" % (base, "  ".join(row)))
        materials.append(base)

    # AN EMBEDDED MATERIAL HAS NO POINTER TO ITSELF. The first run of this found
    # the marker's material and then "nobody in main RAM", which is not a dead
    # end - it means the material is a FIELD of a larger object rather than
    # something allocated separately. So the bytes around it are dumped, and
    # pointers are looked for anywhere in the neighbourhood, which is what
    # finds the container.
    for m in materials:
        print("")
        print("=== the neighbourhood of %08X ===" % m)
        for row in range(-0x40, 0x40, 0x10):
            vals = []
            for c in range(0, 0x10, 4):
                v = w32(blob, m + row + c)
                vals.append("%08X" % (v if v is not None else 0))
            mark = "  <- material base" if row == 0 else ""
            print("  %08X  %s%s" % (m + row, " ".join(vals), mark))

    print("")
    print("=== pointers landing within 0x200 of the material ===")
    near = {}
    lo, hi = materials[0] - 0x200, materials[0] + 0x200
    for cand in range(lo & ~3, hi, 4):
        for x in holders(blob, cand):
            near.setdefault(cand, []).append(x)
    if not near:
        print("  none. The container is not reached by a pointer from main RAM")
        print("  either - it may be an array element reached by index, or held")
        print("  only in DTCM or a register.")
    for cand in sorted(near):
        for x in near[cand][:4]:
            print("  %08X (material%+d) held at %08X" % (cand, cand - materials[0], x))

    for level in range(depth):
        nxt = []
        print("")
        print("=== who holds %s ===" %
              ", ".join("%08X" % m for m in materials))
        for m in materials:
            hs = holders(blob, m)
            if not hs:
                print("  %08X : nobody in main RAM" % m)
                continue
            for x in hs[:10]:
                # Print a little context around the slot, because the offset the
                # pointer sits at is what names the field in its owner.
                ctx = []
                for d in (-8, -4, 0, 4, 8):
                    v = w32(blob, x + d)
                    ctx.append("%+d:%08X" % (d, v if v is not None else 0))
                print("  %08X held at %08X   %s" % (m, x, " ".join(ctx)))
                nxt.append(x)
            if len(hs) > 10:
                print("  ... %d more holders of %08X (print limit)"
                      % (len(hs) - 10, m))
        if not nxt:
            break
        materials = nxt


main()
