"""Is the top-screen portrait ring (the player slot-colour ring) an OAM sprite, live.

Every previous answer came from a recorded melonDS capture that does not record
POWCNT1, so nobody could say which engine was on which screen. The running port
can be asked both questions at once.
"""
import json
import socket
import sys

HOST, PORT = "127.0.0.1", 19842


class S:
    def __init__(self):
        self.k = socket.create_connection((HOST, PORT), timeout=15.0)
        self.b = b""

    def cmd(self, o):
        self.k.sendall((json.dumps(o) + "\n").encode())
        while b"\n" not in self.b:
            c = self.k.recv(1 << 20)
            if not c:
                raise RuntimeError("closed")
            self.b += c
        ln, self.b = self.b.split(b"\n", 1)
        return json.loads(ln.decode())

    def mem(self, addr, ln):
        r = self.cmd({"cmd": "read_mem", "addr": addr, "len": ln})
        return bytes.fromhex(r["hex"])

    def io(self, addr, width=32):
        return self.cmd({"cmd": "read_io", "addr": addr, "width": width})["value"]


def decode(oam, i):
    """One OAM entry. Same field layout objsprite.cpp decodes."""
    o = i * 8
    a0 = oam[o] | (oam[o + 1] << 8)
    a1 = oam[o + 2] | (oam[o + 3] << 8)
    a2 = oam[o + 4] | (oam[o + 5] << 8)
    y = a0 & 0xFF
    shape = (a0 >> 14) & 3
    affine = (a0 >> 8) & 1
    disabled = (not affine) and ((a0 >> 9) & 1)
    x = a1 & 0x1FF
    if x >= 256:
        x -= 512
    size = (a1 >> 14) & 3
    tile = a2 & 0x3FF
    prio = (a2 >> 10) & 3
    pal = (a2 >> 12) & 0xF
    dims = {
        0: [(8, 8), (16, 16), (32, 32), (64, 64)],
        1: [(16, 8), (32, 8), (32, 16), (64, 32)],
        2: [(8, 16), (8, 32), (16, 32), (32, 64)],
    }
    w, h = dims.get(shape, [(0, 0)] * 4)[size]
    return dict(i=i, x=x, y=y, w=w, h=h, tile=tile, pal=pal, prio=prio,
                disabled=bool(disabled), affine=bool(affine))


def main():
    s = S()
    powcnt1 = s.io(0x04000304, 16)
    a_on_top = bool(powcnt1 & 0x8000)
    print(f"POWCNT1 = 0x{powcnt1:04X}   display swap bit 15 = {int(a_on_top)}")
    print(f"  -> engine A (main) is on the {'TOP' if a_on_top else 'BOTTOM'} screen")
    print(f"  -> the TOP screen is engine {'A/main' if a_on_top else 'B/sub'}")

    for name, base in (("A/main", 0x04000000), ("B/sub", 0x04001000)):
        d = s.io(base, 32)
        layers = [(d >> (8 + n)) & 1 for n in range(5)]
        print(f"engine {name:7} DISPCNT=0x{d:08X}  "
              f"BG0={layers[0]} BG1={layers[1]} BG2={layers[2]} "
              f"BG3={layers[3]} OBJ={layers[4]}"
              f"{'   <- sprite layer OFF' if not layers[4] else ''}")

    for name, base in (("main", 0x07000000), ("sub", 0x07000400)):
        oam = s.mem(base, 0x400)
        live = [decode(oam, i) for i in range(128)]
        live = [e for e in live if not e["disabled"] and e["w"]]
        pals = sorted({e["pal"] for e in live})
        print(f"\nOAM {name}: {len(live)} live sprites, palettes {pals}")
        colour = [e for e in live if 4 <= e["pal"] <= 7]
        print(f"  in the player-colour range 4..7: {len(colour)}")
        for e in sorted(live, key=lambda e: (e["y"], e["x"]))[:24]:
            mark = "  <-- player colour" if 4 <= e["pal"] <= 7 else ""
            print(f"    pal {e['pal']:2}  ({e['x']:4},{e['y']:3})  "
                  f"{e['w']:2}x{e['h']:<2} tile {e['tile']:4} prio {e['prio']}{mark}")


if __name__ == "__main__":
    sys.exit(main())
