"""Dump the real string bank so the C++ decoder can be checked against it.

WHY THIS EXISTS. port/frontend/test_strings.cpp builds its own Huffman bank, so
it proves the algorithm is self-consistent - bit order, child arithmetic, the
terminator, the capacity rule - but it cannot prove the C++ agrees with the
GAME. Only the real bank can do that, and the real bank lives in rebuilt RAM
from a capture, which is a 4MB input that does not belong in the repository.

So this writes the fixture to a scratch directory instead, and
test_strings_real.cpp reads it if it is there and skips if it is not. The
repository stays small, the check is available to anyone with the capture, and
nothing pretends to have run when it did not.

    python tools/dump_string_bank_fixture.py [outdir]

Writes two files:

    ram.bin         main RAM, rebuilt, raw
    expected.bin    u32 count, then per entry: u32 id, u32 len, len bytes

The expected strings come from config/strings.json, which
tools/export_strings.py produced and self-validated against character names
captured independently from framebuffers. That is the chain that makes this a
check against the game rather than against itself.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import reconstruct_ram as rr  # noqa: E402

CAPTURE = ROOT / "out" / "work" / "session_live_01" / "inst0"
FRAME = 20000
STRINGS = ROOT / "config" / "strings.json"


def main() -> int:
    outdir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "out" / "work" / "string_fixture"
    outdir.mkdir(parents=True, exist_ok=True)

    if not CAPTURE.exists():
        print("no capture at %s - nothing to dump" % CAPTURE, file=sys.stderr)
        return 1

    ram, frame = rr.rebuild(CAPTURE, FRAME)
    print("rebuilt %d bytes of RAM at 0x%08X, frame %s"
          % (len(ram), rr.MAIN_RAM_BASE, frame))

    (outdir / "ram.bin").write_bytes(bytes(ram))

    table = json.loads(STRINGS.read_text(encoding="utf-8"))["strings"]
    entries = []
    for key, value in table.items():
        sid = int(key, 16)
        # config/strings.json holds the bytes decoded as latin1, so encoding
        # back to latin1 recovers exactly what the decoder produced. Anything
        # that will not round-trip is a bug in the exporter, not something to
        # paper over here.
        entries.append((sid, value.encode("latin1")))
    entries.sort()

    blob = bytearray(struct.pack("<I", len(entries)))
    for sid, raw in entries:
        blob += struct.pack("<II", sid, len(raw))
        blob += raw
    (outdir / "expected.bin").write_bytes(bytes(blob))

    print("wrote %s" % (outdir / "ram.bin"))
    print("wrote %s (%d strings)" % (outdir / "expected.bin", len(entries)))
    print("\nnow run:")
    print("  g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_strings_real.cpp"
          " port/frontend/strings.cpp -o test_strings_real")
    print("  ./test_strings_real %s" % outdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
