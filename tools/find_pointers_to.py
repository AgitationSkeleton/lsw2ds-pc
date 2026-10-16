"""Find every aligned word in main RAM that holds a given address, and repeat.

WHY. Reading a structure downwards is easy - follow the pointers you have. This
answers the opposite question, "who points AT this", which is what you need when
you have found an object by its contents and want the thing that owns it.

It is what the top-screen portrait fix needed. The top-screen portrait's face texture object was
located by its VRAM offset (tools/find_tex_object.py), and the sprite that draws
it holds that pointer at +0xD4, and the HUD that owns the sprite holds the
sprite pointer at +0x58 or +0x60. Walking up that chain by hand means a scan,
a subtraction and another scan; guessing the owning object instead cost one wasted run,
because a state pointer that looked right turned out to own nothing.

Each level prints the raw hit AND the candidate object address for the offsets
that matter, so the answer is readable without arithmetic in your head.

Usage:
    python tools/find_pointers_to.py 0x021FDE44
    python tools/find_pointers_to.py 0x021FDE44 --depth 3 --port 19842
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


def snapshot(port):
    """All of main RAM, once. Every level searches this same copy."""
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
        print("NOTE: %d chunks came back short and were zero-filled; a miss in "
              "those regions is not evidence." % short)
    return bytes(blob)


def holders(blob, value):
    need = value.to_bytes(4, "little")
    out = []
    for off in range(0, len(blob) - 4, 4):
        if blob[off:off + 4] == need:
            out.append(START + off)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    target = int(sys.argv[1], 0)
    depth = 2
    port = 19842
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--depth":
            depth = int(args[i + 1], 0)
            i += 2
        elif args[i] == "--port":
            port = int(args[i + 1], 0)
            i += 2
        else:
            i += 1

    blob = snapshot(port)

    # The struct offsets this project has established, so a hit can be read as
    # "an object of a known kind" instead of a bare address.
    KNOWN = {0xD4: "sprite +0xD4 (texture)", 0xD8: "sprite +0xD8 (palette)",
             0x54: "hud +0x54 (ring, player 1)", 0x58: "hud +0x58 (face, player 1)",
             0x5C: "hud +0x5C (ring, player 2)", 0x60: "hud +0x60 (face, player 2)"}

    frontier = [target]
    for level in range(depth):
        print("")
        print("=== level %d: who holds %s ===" %
              (level + 1, ", ".join("%08X" % v for v in frontier)))
        nxt = []
        for v in frontier:
            hits = holders(blob, v)
            if not hits:
                print("  %08X : nobody" % v)
                continue
            for h in hits[:12]:
                notes = []
                for off, name in KNOWN.items():
                    notes.append("%s -> object %08X" % (name, h - off))
                print("  %08X held at %08X" % (v, h))
                for n in notes:
                    print("        if %s" % n)
                nxt.append(h)
            if len(hits) > 12:
                print("  ... %d more holders of %08X not shown (print limit)"
                      % (len(hits) - 12, v))
        if not nxt:
            break
        # Chase the OBJECT the pointer most likely belongs to, not the raw slot:
        # for the next level the interesting value is the object base, and the
        # sprite case (+0xD4) is the one this project actually walks.
        frontier = [h - 0xD4 for h in nxt]


main()
