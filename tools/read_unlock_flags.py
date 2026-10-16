"""Read the two unlock bitfields out of the running port.

WHY THIS EXISTS. Earlier analysis found the cheat-code system: 108 six-symbol codes at
0x020FED9C, a validator at 0x020787B0, and an effect that is a bit index into
two parallel bitfields hanging off the global progress block at 0x02104D90.

What the ROM does NOT say is which of the two arrays carries which meaning. The
dispatcher tests the first and then sets one or the other depending on a byte in
the roster table, and reading that byte's full enumeration is work that has not
been done. So rather than reason about it, the port's "everything unlocked from
the start" is built from a MEASURED pair: boot with a save that has the four codes entered,
read the arrays, and use what is there.

    python tools/read_unlock_flags.py                 # both arrays, as words
    python tools/read_unlock_flags.py --bits          # also list the set bits
    python tools/read_unlock_flags.py --save out.json # record for comparison
    python tools/read_unlock_flags.py --diff a.json b.json   # no port needed

THE PORT MUST BE RUNNING WITH ITS DEBUG SURFACE ENABLED. This talks to it over
TCP and never touches the window, so it does not steal focus.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys

HOST = "127.0.0.1"
PORT = 19842

# The progress block, and the two array bases. Every one of these came out of
# the accessors themselves rather than from a search.
FLAG_BLOCK = 0x02104D90
ARRAY_A = 0x44CC   # tested by 0x02086534, set by 0x02086458
ARRAY_B = 0x44DC   # tested by 0x02086588, set by 0x02086558

# FOUR WORDS, and the reason is geometric rather than assumed: the two bases are
# 0x10 apart, so the first array cannot be longer than four words without
# running into the second, and 108 codes need exactly four (128 bits). The
# second array's extent is NOT bounded by anything - four is what is read
# because it is the matching size, and that is a choice.
WORDS = 4

MAINRAM = 0x02000000


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


def read_arrays(surface):
    """Both arrays, as lists of WORDS 32-bit values.

    Reads the whole of main RAM in one request rather than issuing eight small
    reads. The surface handles ONE command per frame, so eight reads
    are eight frames during which the game is running and the arrays could
    change underneath the read - which would produce a pair that never existed.
    One request is one consistent snapshot.
    """
    reply = surface.command(cmd="read_region", region="mainram")
    if "hex" not in reply:
        raise RuntimeError("read_region failed: %s" % reply)
    blob = bytes.fromhex(reply["hex"])

    def grab(base):
        off = FLAG_BLOCK + base - MAINRAM
        if off + WORDS * 4 > len(blob):
            raise RuntimeError("main RAM read is too short: %d bytes" % len(blob))
        return [int.from_bytes(blob[off + i * 4: off + i * 4 + 4], "little")
                for i in range(WORDS)]

    return grab(ARRAY_A), grab(ARRAY_B)


def set_bits(words):
    """Which code indices are set, in table order."""
    out = []
    for w, value in enumerate(words):
        for b in range(32):
            if value & (1 << b):
                out.append(w * 32 + b)
    return out


def show(label, words, want_bits):
    print("%s  %s" % (label, " ".join("%08X" % w for w in words)))
    if want_bits:
        bits = set_bits(words)
        print("    %d set: %s" % (len(bits), bits if bits else "-"))


def code_names():
    """Decode the code table so a set bit can be named.

    Returns index -> six-character code, or an empty dict when the ARM9 image is
    not to hand. Purely a convenience: the numbers are the finding, the names
    just make the output readable.
    """
    path = "out/raw/arm9_section_0_02000000.bin"
    try:
        data = open(path, "rb").read()
    except OSError:
        return {}
    off = 0x020FED9C - MAINRAM
    out = {}
    for i in range(108):
        row = data[off + i * 6: off + i * 6 + 6]
        if len(row) < 6 or any(b > 35 for b in row):
            break
        out[i] = "".join(chr(48 + b) if b < 10 else chr(65 + b - 10) for b in row)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bits", action="store_true",
                    help="list the set bit indices, and name them where possible")
    ap.add_argument("--save", metavar="FILE", help="write the pair to a JSON file")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"),
                    help="compare two saved files; does not need a running port")
    args = ap.parse_args()

    if args.diff:
        a = json.load(open(args.diff[0]))
        b = json.load(open(args.diff[1]))
        names = code_names()
        for key in ("a", "b"):
            wa, wb = a[key], b[key]
            gained = sorted(set(set_bits(wb)) - set(set_bits(wa)))
            lost = sorted(set(set_bits(wa)) - set(set_bits(wb)))
            print("array %s" % key.upper())
            print("   %s  ->  %s" % (" ".join("%08X" % w for w in wa),
                                     " ".join("%08X" % w for w in wb)))
            for tag, which in (("gained", gained), ("lost", lost)):
                if which:
                    print("   %s %d: %s" % (tag, len(which),
                          ", ".join("%d(%s)" % (i, names.get(i, "?")) for i in which)))
            if not gained and not lost:
                print("   identical")
        return 0

    try:
        surface = Surface()
    except OSError as exc:
        print("cannot reach the port on %s:%d - %s" % (HOST, PORT, exc),
              file=sys.stderr)
        print("Start it with the debug surface enabled (--serve).", file=sys.stderr)
        return 1

    array_a, array_b = read_arrays(surface)
    print("flag block 0x%08X   A at +0x%04X, B at +0x%04X, %d words each"
          % (FLAG_BLOCK, ARRAY_A, ARRAY_B, WORDS))
    show("A", array_a, args.bits)
    show("B", array_b, args.bits)

    if args.bits:
        names = code_names()
        if names:
            both = sorted(set(set_bits(array_a)) | set(set_bits(array_b)))
            if both:
                print("\nnamed:")
                for i in both:
                    print("   %3d  %s" % (i, names.get(i, "?")))

    if args.save:
        json.dump({"a": array_a, "b": array_b}, open(args.save, "w"), indent=1)
        print("\nwrote %s" % args.save)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
