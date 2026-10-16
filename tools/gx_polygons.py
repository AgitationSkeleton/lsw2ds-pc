"""List the 3D polygons the running port most recently rendered.

WHAT THIS IS FOR - the top-screen character portrait that does not follow a
Free Play switch.

An earlier measurement found that portrait is NEITHER BG TILES NOR SPRITES: engine A's
whole 2KB BG map holds sixteen non-background cells and they are the hearts,
and every OAM entry reads attribute0 = 0x0200, the hidden encoding. Engine A's
DISPCNT has bit 3 set, so BG0 IS THE 3D LAYER. The icon is therefore 3D
geometry submitted each frame, and until now nothing in this tree had ever
looked at the geometry.

The framework already exposes it. `gx_polygons` on the debug server returns the
whole render list with, per polygon, its screen bounding box and its TEXTURE
PARAMETER - and the texture parameter is the answer to the portrait's remaining
question, because bits 0-15 are the texture's VRAM address. If the polygon
drawn in the portrait's corner keeps the same texture address across a
character switch, then the field that picks the head is what needs to change,
and this prints it.

VERIFY THE INSTRUMENT BEFORE BELIEVING ITS SILENCE - a standing rule here, and
this tool follows it: it always prints the TOTAL polygon count first. Zero
polygons in the region means nothing at all if the total is also zero, which
would say the render list was empty when it was asked, not that the portrait is
not 3D.

Usage:
    python tools/gx_polygons.py [port] [--region x0 y0 x1 y1] [--all]

Default region is the top-left corner where the portrait sits.
"""

import json
import socket
import sys

# THE NDS TEXTURE FORMATS, TEXIMAGE_PARAM bits 26-28. Named rather than printed
# as a number because "format 5" and "4x4 compressed" are not equally useful
# when the question is which head is on screen.
FORMATS = {
    0: "none",
    1: "A3I5",
    2: "4-colour",
    3: "16-colour",
    4: "256-colour",
    5: "4x4-compressed",
    6: "A5I3",
    7: "direct-colour",
}


def query(port, cmd):
    s = socket.create_connection(("127.0.0.1", port), timeout=15.0)
    f = s.makefile("rwb")
    f.write((json.dumps(cmd) + chr(10)).encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    return json.loads(line)


def describe(p):
    """One polygon, with the texture parameter taken apart."""
    tex = p["tex_param"]
    # Bits 0-15 are the VRAM offset in 8-byte units: this is the texture's
    # address, and it is the thing that has to differ between two characters.
    addr = (tex & 0xFFFF) << 3
    fmt = FORMATS.get((tex >> 26) & 7, "?")
    w = 8 << ((tex >> 20) & 7)
    h = 8 << ((tex >> 23) & 7)
    # THE RAW PARAMETER IS PRINTED TOO, and it is not clutter: the decoded
    # address is what a human reads, but the raw word is what the guest
    # actually stores and therefore what a memory scan has to search for.
    # VRAM texture space begins at 0x06000000, so the absolute address is
    # given as well - `read_guest_mem.py` takes an absolute address.
    return (
        "  #%-4d sub %-4d verts %d  x %4d..%-4d y %4d..%-4d  "
        "tex %06X (abs %08X) %-14s %3dx%-3d  pal %04X  attr %08X"
        "   raw texparam %08X"
        % (
            p["index"],
            p["submission_index"],
            p["vertex_count"],
            p["min_x"],
            p["max_x"],
            p["min_y"],
            p["max_y"],
            addr,
            0x06000000 + addr,
            fmt,
            w,
            h,
            p["tex_palette"],
            p["attr"],
            tex,
        )
    )


def main():
    port = 19842
    region = (0, 0, 64, 64)
    show_all = False

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--all":
            show_all = True
            i += 1
        elif a == "--region":
            region = tuple(int(x) for x in args[i + 1:i + 5])
            i += 5
        else:
            port = int(a, 0)
            i += 1

    reply = query(port, {"cmd": "gx_polygons"})
    polys = reply.get("polygons", [])
    total = reply.get("count", 0)

    # THE INSTRUMENT'S OWN READING FIRST.
    print("render list: %d polygons" % total)
    if total == 0:
        print("  THE LIST IS EMPTY. Nothing below means anything - this says")
        print("  the renderer had submitted no geometry when it was asked,")
        print("  not that the portrait is not 3D. Try again on a later frame.")
        return

    if show_all:
        chosen = polys
        print("all %d polygons:" % len(chosen))
    else:
        x0, y0, x1, y1 = region
        chosen = [
            p
            for p in polys
            if p["min_x"] < x1 and p["max_x"] >= x0
            and p["min_y"] < y1 and p["max_y"] >= y0
        ]
        print(
            "%d of them touch the region x %d..%d y %d..%d:"
            % (len(chosen), x0, x1, y0, y1)
        )

    for p in chosen:
        print(describe(p))


main()
