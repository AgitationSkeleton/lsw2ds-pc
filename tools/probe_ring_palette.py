"""Player-colour ring: find its colour in palette RAM, and prove it by changing it.

Racing the game for DISPCNT loses - it rewrites the register every frame. Palette
RAM is loaded when a screen is built and then left alone, so a write there holds
long enough to photograph. If recolouring an entry recolours the ring, the ring
is drawn from that palette, and the entry index is exactly what a repalette fix
needs to know.

DS palette RAM, 2 KiB at 0x05000000:
    0x05000000  main BG   512 bytes, 256 entries
    0x05000200  main OBJ  512
    0x05000400  sub  BG   512
    0x05000600  sub  OBJ  512
"""
import json
import socket
import sys

HOST, PORT = "127.0.0.1", 19842
BANKS = [("main BG", 0x05000000), ("main OBJ", 0x05000200),
         ("sub BG", 0x05000400), ("sub OBJ", 0x05000600)]


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

    def mem(self, a, n):
        return bytes.fromhex(self.cmd({"cmd": "read_mem", "addr": a, "len": n})["hex"])

    def poke(self, a, data):
        return self.cmd({"cmd": "write_mem", "addr": a, "hex": data.hex()})


def rgb(v):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31,
            ((v >> 10) & 31) * 255 // 31)


def near(c, want, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(c, want))


def main():
    s = S()
    # slotcolour.h's measured ring strokes, as RGB.
    targets = {"slot1 blue": (0, 113, 251), "slot2 green": (24, 211, 0),
               "slot3 red": (251, 0, 8), "slot4 purple": (186, 0, 251)}

    hits = []
    for name, base in BANKS:
        raw = s.mem(base, 512)
        for i in range(256):
            v = raw[i * 2] | (raw[i * 2 + 1] << 8)
            c = rgb(v)
            for tname, want in targets.items():
                if near(c, want):
                    hits.append((name, base, i, v, c, tname))

    print(f"{len(hits)} palette entries near a known ring stroke\n")
    for name, base, i, v, c, tname in hits:
        print(f"  {name:9} entry {i:3} (bank {i//16:2} slot {i%16:2})  "
              f"0x{v:04X} = rgb{c}   ~ {tname}")

    if len(sys.argv) > 1 and sys.argv[1] == "--poke":
        # Recolour every main-BG hit to pure magenta and see what moves.
        for name, base, i, v, c, tname in hits:
            if name != "main BG":
                continue
            s.poke(base + i * 2, (0x7C1F).to_bytes(2, "little"))
            print(f"  poked main BG entry {i} -> magenta")


if __name__ == "__main__":
    sys.exit(main())
