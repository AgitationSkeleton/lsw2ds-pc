"""Find every material object that carries the arrow's texture parameter.

THE GOAL THIS SERVES, IN PLAIN ENGLISH. The co-op marker arrows must not be
visible in the lobby. Three routes that tried to stop the marker EXISTING are
now measured and closed, so the remaining end is the DRAW.

Earlier tracing identified the arrow as four polygons carrying
`TEXIMAGE_PARAM 0x0803682C`, then followed that word into an ITCM material
bind and read the material's layout:

    +0x04  diffuse/ambient
    +0x08  specular/emission
    +0x0C  POLYGON ATTRIBUTE     <- alpha lives in bits 0x1F0000
    +0x10  TEXIMAGE_PARAM        <- 0x0803682C for the arrow
    +0x14  palette base

and the bind's own first branch is an off switch the cartridge already takes:

    01FFBC60  LDR r4,[r8,#0xB0]     ; the material the RENDER CONTEXT binds
    01FFBC64  LDR r1,[r4,#0xC]
    01FFBC68  TST r1,#0x1F0000      ; the alpha field
    01FFBC6C  BEQ 01FFBCF4          ; ZERO ALPHA -> skip the whole bind

WHY THIS TOOL EXISTS RATHER THAN REUSING THE OLD SCAN. An earlier attempt zeroed
"the asset material's alpha", it held, and nothing changed on screen. A prior
measurement had already shown why that might be: **the asset's material reads alpha 0x1F while
the drawn polygons carry 0x17**, so the object that was poked is a template and
not the one the render context binds. Nobody has enumerated the OTHER objects
carrying the same parameter.

So this finds them ALL - every address whose word equals the parameter - and for
each reports the polygon attribute four bytes earlier and its alpha. The one
whose alpha is 0x17 is the candidate the earlier work never touched.

`find_tex_object.py` cannot be reused: it matches the PORTRAIT's struct shape
(enable byte at +0x09, format at +0x0C) on the low half of +0x10. This is a
different struct and an exact 32-bit match.

READ-ONLY. It prints candidates; poking is `arrow_intervene.py`'s job, and doing
it here would mean a scan that changes what it is measuring.

Usage:
    python tools/find_arrow_material.py [--port 19843] [--param 0x0803682C]
"""

from __future__ import annotations

import json
import socket
import sys

HOST = "127.0.0.1"

# MAIN RAM ONLY, AND THAT IS A LIMIT ON THE ANSWER, NOT A DETAIL.
#
# The earlier watch caught the material at the bind as `r4 = 027E3624`, which is
# DTCM and outside this range entirely - and reading that address at rest shows
# something else, because DTCM is per-frame scratch. So a material live only
# during the draw is invisible to this scan twice over: wrong range, wrong
# moment.
#
# "One candidate" from this tool therefore means "one in main RAM", and the
# report says so. Reading it as "there is only one material" is the mistake this
# project keeps making - treating an absence as evidence without asking whether
# the instrument could see it - and an earlier write-up came within one
# sentence of making it.
START = 0x02000000
END = 0x02400000
CHUNK = 0x10000

MAT_ATTR = 0x0C          # the polygon attribute, relative to the object
MAT_PARAM = 0x10         # where the texture parameter lives
ALPHA_MASK = 0x001F0000
ALPHA_SHIFT = 16

DEFAULT_PARAM = 0x0803682C
DRAWN_ALPHA = 0x17       # what was measured on the polygons themselves
TEMPLATE_ALPHA = 0x1F    # what the asset's material reads - the one already tried


def cmd(port, obj):
    s = socket.create_connection((HOST, port), timeout=30.0)
    try:
        f = s.makefile("rwb")
        f.write((json.dumps(obj) + "\n").encode())
        f.flush()
        line = f.readline().decode()
        return json.loads(line) if line.strip() else {}
    finally:
        s.close()


def read(port, addr, ln):
    h = cmd(port, {"cmd": "read_mem", "cpu": 9, "addr": addr, "len": ln}).get("hex", "")
    return bytes.fromhex(h) if h else b""


def main() -> int:
    port = 19843
    param = DEFAULT_PARAM
    args = sys.argv[1:]
    start, end, chunk = START, END, CHUNK
    for i, a in enumerate(args):
        if a == "--port" and i + 1 < len(args):
            port = int(args[i + 1], 0)
        if a == "--param" and i + 1 < len(args):
            param = int(args[i + 1], 0)
        # THE RANGE IS AN ARGUMENT NOW, because it was found that the object that
        # matters is a field of a DTCM render context and main RAM could never
        # have contained it. A scanner whose range is a hidden constant reports
        # absences it was never able to see.
        if a == "--start" and i + 1 < len(args):
            start = int(args[i + 1], 0)
        if a == "--end" and i + 1 < len(args):
            end = int(args[i + 1], 0)
        if a == "--chunk" and i + 1 < len(args):
            chunk = int(args[i + 1], 0)

    needle = param.to_bytes(4, "little")
    print("scanning port %d, %08X-%08X, for material objects carrying %08X"
          % (port, start, end, param))
    print("  (the object is 0x10 before the hit; its polygon attribute is at +0x0C)")

    hits = []
    refused = 0
    for base in range(start, end, chunk):
        blob = read(port, base, chunk)
        if len(blob) != chunk:
            # A REFUSED READ IS NOT AN EMPTY REGION. Say so rather than letting
            # a gap in coverage read as "the object is not there".
            refused += 1
            continue
        off = blob.find(needle)
        while off >= 0:
            if off % 4 == 0:
                hits.append(base + off)
            off = blob.find(needle, off + 1)

    if refused:
        print("  NOTE: %d chunk(s) could not be read - coverage is incomplete, and"
              % refused)
        print("        an object in those regions would not appear here.")

    if not hits:
        print("  no occurrence of the parameter found. If the peer is not")
        print("  ADMITTED the marker is not on screen and this is a correct")
        print("  negative that says nothing.")
        return 1

    print()
    print("  %-10s %-10s %-10s %s" % ("param at", "object", "poly attr", "alpha"))
    for h in hits:
        obj = h - MAT_PARAM
        attr_b = read(port, obj + MAT_ATTR, 4)
        if len(attr_b) != 4:
            print("  %08X   %08X   READ REFUSED - not a zero" % (h, obj))
            continue
        attr = int.from_bytes(attr_b, "little")
        alpha = (attr & ALPHA_MASK) >> ALPHA_SHIFT
        note = ""
        if alpha == DRAWN_ALPHA:
            note = "  <- matches the DRAWN polygons; never poked before"
        elif alpha == TEMPLATE_ALPHA:
            note = "  <- the template already zeroed earlier, to no effect"
        print("  %08X   %08X   %08X   0x%02X%s" % (h, obj, attr, alpha, note))

    print()
    print("  %d candidate(s) in %08X-%08X. The alpha field is what"
          % (len(hits), start, end))
    print("  the ITCM bind tests at 01FFBC68; zeroing it makes the cartridge")
    print("  skip the bind by its own branch rather than by anything invented.")
    print()
    print("  THIS IS NOT A COUNT OF THE GAME'S MATERIALS. A bind watch saw")
    print("  the bound material at 027E3624 - DTCM, outside this range, and")
    print("  per-frame scratch that reads as something else at rest. A material")
    print("  live only during the draw cannot appear here.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
