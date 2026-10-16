"""Dump the most recent ARM9 writes into the 3D register window.

WHY. The top-screen portrait is two polygons whose TEXIMAGE_PARAM never changes
across a character switch. To find the guest routine that submits
them, the first thing needed is WHICH PORT the parameter is written to: the
direct command port, or the packed GXFIFO. A direct-port write comes from a CPU
store and can therefore be watched with LSW2_WRITE_WATCH, which reports pc and
lr; a GXFIFO write may instead be a DMA'd display list, which no store watch
will ever see. Those two answers lead to completely different next steps, so it
is worth one cheap query to tell them apart.

The framework keeps an always-on ring of these writes with the engine's gate
state at each one. This walks it backwards from the newest.

Usage:
    python tools/gx_writes.py [port] [--last N] [--value 0xHHHHHHHH]
"""

import json
import socket
import sys

# The addresses worth naming. Everything else is printed as a bare number.
PORTS = {
    0x04000400: "GXFIFO",
    0x04000440: "MTX_MODE",
    0x04000444: "MTX_PUSH",
    0x04000448: "MTX_POP",
    0x04000450: "MTX_IDENTITY",
    0x04000470: "MTX_TRANS",
    0x040004A4: "POLYGON_ATTR",
    0x040004A8: "TEXIMAGE_PARAM",
    0x040004AC: "PLTT_BASE",
    0x04000480: "BEGIN_VTXS",
    0x04000484: "END_VTX",
    0x04000488: "VTX_16",
    0x040004C0: "NORMAL",
    0x040004A0: "COLOR",
    0x04000540: "SWAP_BUFFERS",
    0x04000600: "GXSTAT",
}


def q(port, cmd):
    s = socket.create_connection(("127.0.0.1", port), timeout=15.0)
    f = s.makefile("rwb")
    f.write((json.dumps(cmd) + chr(10)).encode())
    f.flush()
    line = f.readline().decode()
    s.close()
    return json.loads(line)


def main():
    port = 19842
    last = 400
    want = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--last":
            last = int(args[i + 1], 0)
            i += 2
        elif args[i] == "--value":
            want = int(args[i + 1], 0) & 0xFFFFFFFF
            i += 2
        else:
            port = int(args[i], 0)
            i += 1

    # The ring's newest ordinal. gx_write_sample is addressed by absolute
    # ordinal from reset, so the walk has to start from wherever it is now -
    # and asking with count 0 is how the server reports that ordinal.
    total = q(port, {"cmd": "gx_write_sample", "count": 0}).get("latest")
    if not total:
        print("the write ring reports no entries - nothing below would mean")
        print("anything, so nothing was read.")
        return

    print("3D register writes so far: %d" % total)
    shown = 0
    hits = 0
    missed = 0
    for n in range(total - 1, max(-1, total - 1 - last), -1):
        e = q(port, {"cmd": "gx_write_sample", "count": n})
        addr = e.get("addr")
        if addr is None:
            # ALREADY EVICTED. This is the normal case on a free-running
            # target and it is counted, not shrugged off - see the warning
            # printed at the end for why that matters.
            missed += 1
            continue
        val = e.get("val", 0)
        if want is not None and val != want:
            continue
        hits += 1
        name = PORTS.get(addr, "")
        print(
            "  #%-8d %08X %-14s = %08X  w=%s  geom=%s gxstat=%08X pipe=%s"
            % (n, addr, name, val, e.get("width"), e.get("geometry_enabled"),
               e.get("gxstat", 0), e.get("pipe_level"))
        )
        shown += 1
        if shown >= 60:
            print("  ... stopping at 60 lines; this is the print limit, not")
            print("  the end of the ring.")
            break
    # THE INSTRUMENT HAS TO REPORT ITS OWN BLINDNESS, and this one is nearly
    # blind by construction. The ring is small, the guest writes thousands of
    # 3D registers per frame, and every entry costs one socket round-trip - so
    # by the time the walk reaches an ordinal the engine has usually overwritten
    # it. A run that read almost nothing must not be reported as a run that
    # found nothing; the first version of this file printed a confident "does
    # not appear, therefore it is DMA" over a 39-out-of-40 eviction rate.
    after = q(port, {"cmd": "gx_write_sample", "count": 0}).get("latest", total)
    asked = min(last, total)
    print("")
    print("asked for %d entries, %d were already evicted, %d readable"
          % (asked, missed, asked - missed))
    print("the ring advanced by %d writes DURING the walk" % (after - total))
    if missed > asked // 10:
        print("MORE THAN A TENTH WAS UNREADABLE - this walk cannot support a")
        print("negative conclusion. A value that did not show up here may")
        print("simply have been overwritten before it could be read.")
    if want is not None and hits == 0:
        print("value %08X was not seen in what could actually be read." % want)


main()
