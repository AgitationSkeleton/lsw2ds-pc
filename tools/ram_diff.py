"""Find the memory that holds a piece of game state, by changing only that.

WHY A DIFF AND NOT A SEARCH. tools/ram_find.py locates known bytes in memory,
which works when the game keeps something byte-for-byte. LSW2 does not: loading a save
whose unlock bytes are 0xFF runs produces no matching bytes anywhere in memory, so
the save is unpacked or transformed on load and its RAM form is unknown.

What IS known is how to change the state from outside - load a different save -
and that is enough. Capture memory in one state, capture it in another, and the
bytes that differ are candidates. The difficulty is that almost everything else
differs too: timers, particle positions, allocator state, the frame counter.

So candidates are filtered by STABILITY. Take several snapshots within each state
and keep only addresses that:

    - are identical across every snapshot of state A, AND
    - are identical across every snapshot of state B, AND
    - differ between A and B

A frame counter fails the first test. A random allocation fails it too. What
survives is memory that is constant while the game runs and depends on the thing
that was changed. That is the definition of the state being looked for.

This is the standard cheat-search method, and it is worth having as a tool rather
than as a one-off: the same procedure finds studs, level flags, roster state, and
whatever the next game needs.

    # capture several moments in one state
    python tools/ram_diff.py snap out/work/ram/locked-1
    python tools/ram_diff.py snap out/work/ram/locked-2

    # ... restart with the other save, capture again ...
    python tools/ram_diff.py snap out/work/ram/unlocked-1
    python tools/ram_diff.py snap out/work/ram/unlocked-2

    # what changed, and only what changed
    python tools/ram_diff.py diff --a "out/work/ram/locked-*" \
                                 --b "out/work/ram/unlocked-*"

Snapshots are raw region dumps, so they are also readable by anything else.
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import socket
import sys

HOST, PORT = "127.0.0.1", 19842

REGION_BASE = {
    "mainram": 0x02000000,
    "wramshared": 0x03000000,
    "wram7": 0x03800000,
    "itcm": 0x01FF8000,
    "dtcm": 0x027E0000,
}


def read_region(region: str) -> bytes:
    sock = socket.create_connection((HOST, PORT), timeout=60.0)
    try:
        sock.sendall((json.dumps({"cmd": "read_region", "region": region})
                      + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("the port closed the connection")
            buf += chunk
        reply = json.loads(buf.partition(b"\n")[0].decode())
    finally:
        sock.close()
    if "hex" not in reply:
        raise SystemExit("read_region failed: %s" % reply)
    return bytes.fromhex(reply["hex"])


def cmd_snap(args) -> int:
    out = pathlib.Path(args.path)
    out.parent.mkdir(parents=True, exist_ok=True)
    data = read_region(args.region)
    out.write_bytes(data)
    print("wrote %s (%d bytes of %s)" % (out, len(data), args.region))
    return 0


def load_group(pattern: str) -> list[bytes]:
    paths = sorted(glob.glob(pattern))
    if not paths:
        raise SystemExit("no snapshots match %r" % pattern)
    blobs = [pathlib.Path(p).read_bytes() for p in paths]
    sizes = {len(b) for b in blobs}
    if len(sizes) != 1:
        raise SystemExit("snapshots differ in size: %s" % sorted(sizes))
    print("  %-40s %d snapshot(s)" % (pattern, len(blobs)))
    return blobs


def cmd_diff(args) -> int:
    print("loading:")
    a = load_group(args.a)
    b = load_group(args.b)
    if len(a[0]) != len(b[0]):
        raise SystemExit("the two groups are different sizes")
    if len(a) < 2 or len(b) < 2:
        print("warning: fewer than 2 snapshots in a group - nothing filters out\n"
              "         unstable memory, so expect a great deal of noise",
              file=sys.stderr)

    size = len(a[0])
    base = REGION_BASE.get(args.region, 0)
    a0, b0 = a[0], b[0]

    runs = []
    run_start = -1
    for i in range(size):
        va = a0[i]
        stable = all(s[i] == va for s in a[1:])
        if stable:
            vb = b0[i]
            stable = all(s[i] == vb for s in b[1:]) and vb != va
        if stable:
            if run_start < 0:
                run_start = i
        elif run_start >= 0:
            runs.append((run_start, i))
            run_start = -1
    if run_start >= 0:
        runs.append((run_start, size))

    total = sum(e - s for s, e in runs)
    print("\n%d candidate byte(s) in %d run(s)" % (total, len(runs)))
    if not runs:
        print("Nothing is both stable and different. Either the state does not\n"
              "live in this region, or a snapshot was taken while it was still\n"
              "settling - try again from the same point in each state.")
        return 2

    # Longest runs first: a bitfield or a table is far more interesting than a
    # lone byte that happened to survive the filter.
    runs.sort(key=lambda r: r[1] - r[0], reverse=True)
    shown = runs[:args.top]
    print("\nlongest runs:")
    for s, e in shown:
        n = e - s
        print("  %08X..%08X  %4d bytes" % (base + s, base + e - 1, n))
        width = min(n, 24)
        print("      A: %s" % a0[s:s + width].hex(" "))
        print("      B: %s" % b0[s:s + width].hex(" "))
    if len(runs) > len(shown):
        print("  ... and %d shorter run(s)" % (len(runs) - len(shown)))

    if args.out:
        path = pathlib.Path(args.out)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(
            {"region": args.region, "base": base,
             "runs": [{"addr": base + s, "len": e - s,
                       "a": a0[s:e].hex(), "b": b0[s:e].hex()}
                      for s, e in runs]}, indent=2), encoding="utf-8")
        print("\nwrote %s" % path)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(required=True)

    p = sub.add_parser("snap", help="dump a region to a file")
    p.add_argument("path")
    p.add_argument("--region", default="mainram")
    p.set_defaults(fn=cmd_snap)

    p = sub.add_parser("diff", help="stable-and-different bytes between groups")
    p.add_argument("--a", required=True, help="glob for state A snapshots")
    p.add_argument("--b", required=True, help="glob for state B snapshots")
    p.add_argument("--region", default="mainram")
    p.add_argument("--top", type=int, default=15)
    p.add_argument("--out", help="write every run to this JSON file")
    p.set_defaults(fn=cmd_diff)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
