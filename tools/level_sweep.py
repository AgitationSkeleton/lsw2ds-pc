"""Warp to every level in turn and capture what it actually looks like.

STATUS: NOT WORKING YET. Read this before trusting it.

The warp it performs is correct - the same writes done from the cheats tab load
a level reliably, verified on screen. What does not work is driving it from a
script for long enough to sweep 73 levels: after a few hundred commands the
debug surface stops answering and every later read times out with no other
symptom. That happens whether the commands come over many short connections
(pilot.py style) or one long-lived one, so it is command volume or a frontend
stall rather than connection churn, and it is not diagnosed.

Left in the tree because the protocol below IS the verified warp and is worth
keeping written down, and because the failure is in the plumbing rather than in
anything it claims about the game. Do not read a clean run out of it until the
surface stops dropping out.


WHY. port/frontend/levels.h names 73 levels, and most of those names are
DERIVED - built from the asset names a level's record points at, because nothing
in the ROM says what a level is called. The derivation is honest about being a
guess and it is often wrong: id 0 is labelled "Endor" and loads somewhere that
is plainly not Endor.

Now that the warp works, the game itself can settle it. Warping to a level and
looking at it is a far better source of truth than inferring from asset names,
and this makes doing that for all 73 a single command instead of 73 rounds of
clicking.

Requires the port running with:
    --serve                        the debug surface
    NDS_DEBUG_ALLOW_WRITES=1       writes opted in (port/patches/0003)
and the game already inside a level, because the warp works by replacing the
running level state - there is nothing to replace from a menu.

    python tools/level_sweep.py --out out/work/levels
    python tools/level_sweep.py --only 0,4,5,9,14
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 19842

# The manager is the state machine's slot table; slot 0 is the
# level state, and the pump acts on the status byte on its next tick.
MANAGER = 0x02102900
SLOT_RESULT = 0x00
SLOT_ARGS = 0x48
SLOT_STATUS = 0x90
LEVEL_KIND = 4
STATE_KIND = 0x04
STATE_ARG_A = 0x08
STATE_LEVEL = 0x0C
STATE_ARG_C = 0x10


class Surface:
    def __init__(self):
        # LONGER THAN THE SERVER'S OWN WAIT, which is 30 seconds before it
        # gives up on the frontend and answers "frontend busy or frozen".
        # Matching that exactly meant the client hit its own timeout at the
        # same moment, so it never received the retryable error - the busy
        # handling below could never fire and every long load looked like a
        # dead connection.
        self.sock = socket.create_connection((HOST, PORT), timeout=90)
        self.buf = b""

    def command(self, retries=60, **kw):
        """Send one command, waiting out a busy frontend.

        Commands are executed on the frontend thread between frames, so
        anything that makes a frame take a long time - loading a level, which
        is what this tool spends its time doing - is answered with
        "frontend busy or frozen" rather than a result. That is a WAIT, not a
        failure. Treating it as one is why the first version of this tool fell
        over on its very first warp."""
        for _ in range(retries):
            self.sock.sendall((json.dumps(kw) + "\n").encode())
            while b"\n" not in self.buf:
                chunk = self.sock.recv(1 << 20)
                if not chunk:
                    raise RuntimeError("the port closed the connection")
                self.buf += chunk
            line, _, self.buf = self.buf.partition(b"\n")
            reply = json.loads(line.decode())
            error = reply.get("error", "")
            if "busy" not in error and "frozen" not in error:
                return reply
            time.sleep(0.5)
        raise SystemExit("the frontend stayed busy through %d attempts at %s"
                         % (retries, kw.get("cmd")))

    def read(self, addr, n):
        return bytes.fromhex(self.command(cmd="read_mem", addr=addr, len=n)["hex"])

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def write32(self, addr, value):
        reply = self.command(cmd="write_mem", addr=addr, value=value, width=32)
        if "ok" not in reply:
            raise SystemExit("write_mem refused: %s\n"
                             "Start the port with NDS_DEBUG_ALLOW_WRITES=1."
                             % reply)

    def press(self, key, frames=3):
        """Tap a DS button on THIS connection.

        Deliberately not shelling out to pilot.py: that opens a fresh socket per
        command, and the surface serves one client at a time - a few hundred
        connect/disconnect cycles leaves it not accepting any more, after which
        every later read times out with no other symptom. Doing everything on
        one connection avoids the churn entirely."""
        self.command(cmd="frontend_input", action="key", key=key, down=True)
        for _ in range(frames):
            self.command(cmd="frontend_stats")
        self.command(cmd="frontend_input", action="key", key=key, down=False)
        for _ in range(frames):
            self.command(cmd="frontend_stats")

    def framebuffer(self, engine="A"):
        r = self.command(cmd="framebuffer", engine=engine)
        return r["w"], r["h"], bytes.fromhex(r["rgb"])


def load_levels(header: pathlib.Path):
    text = header.read_text(encoding="utf-8")
    rows = re.findall(r'\{\s*(\d+),\s*(\d+),\s*"([^"]*)",\s*"([^"]*)"\}', text)
    return [(int(i), int(rec), name, src) for i, rec, name, src in rows]


def save_ppm(path: pathlib.Path, w, h, rgb):
    """PPM rather than PNG so this has no dependency to install. Anything that
    matters opens it, and converting later is trivial."""
    path.write_bytes(b"P6\n%d %d\n255\n" % (w, h) + rgb)


STATUS_BUILT = 3


def settled(s: Surface) -> bool:
    """The pump leaves the status byte at 3 once a state is built and entered.
    Requesting a new level before then tears down a half-built state, which
    crashes the guest - it was observed running off into instruction words."""
    return (s.u32(MANAGER + SLOT_STATUS) & 0xFF) == STATUS_BUILT


def wait_settled(s: Surface, timeout=20.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if settled(s):
            return True
        time.sleep(0.25)
    return False


def warp(s: Surface, index: int) -> bool:
    if not settled(s):
        return False
    state = s.u32(MANAGER + SLOT_RESULT)
    if not (0x02000000 <= state < 0x02400000):
        return False
    kind = s.u32(state + STATE_KIND)
    if kind != LEVEL_KIND:
        return False
    arg_a = s.u32(state + STATE_ARG_A)
    arg_c = s.u32(state + STATE_ARG_C)
    s.write32(MANAGER + SLOT_ARGS + 0x00, kind)
    s.write32(MANAGER + SLOT_ARGS + 0x04, arg_a)
    s.write32(MANAGER + SLOT_ARGS + 0x08, index)
    s.write32(MANAGER + SLOT_ARGS + 0x0C, arg_c)
    status = s.u32(MANAGER + SLOT_STATUS)
    s.write32(MANAGER + SLOT_STATUS, (status & 0xFFFFFF00) | 1)
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="out/work/levels")
    ap.add_argument("--header", default="port/frontend/levels.h")
    ap.add_argument("--only", help="comma-separated level ids")
    ap.add_argument("--boot", type=int, default=0,
                    help="tap Start/A this many times first, to get into a level")
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to let a level finish loading")
    args = ap.parse_args()

    levels = load_levels(pathlib.Path(args.header))
    if not levels:
        raise SystemExit("no levels parsed from %s" % args.header)
    if args.only:
        wanted = {int(x) for x in args.only.split(",")}
        levels = [l for l in levels if l[0] in wanted]

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    try:
        s = Surface()
    except OSError as exc:
        print("cannot reach the port: %s" % exc, file=sys.stderr)
        return 1

    log = []
    if args.boot:
        # Get from the title screen into a level, on this same connection.
        print("booting in...")
        for _ in range(args.boot):
            s.press("start")
            s.press("a")
        time.sleep(3.0)

    if not wait_settled(s):
        print("the game never settled - is it inside a level?", file=sys.stderr)
        return 1

    for level_id, record, name, source in levels:
        if not wait_settled(s):
            print("  id %-3d SKIPPED - the previous level never settled" % level_id)
            continue
        if not warp(s, level_id):
            print("  id %-3d SKIPPED - not in a level state right now" % level_id)
            continue
        time.sleep(args.settle)
        wait_settled(s)
        reached = -1
        state = s.u32(MANAGER + SLOT_RESULT)
        if 0x02000000 <= state < 0x02400000:
            reached = s.u32(state + STATE_LEVEL)
        w, h, rgb = s.framebuffer("A")
        path = out / ("level-%02d.ppm" % level_id)
        save_ppm(path, w, h, rgb)
        ok = "ok" if reached == level_id else "REACHED %d" % reached
        print("  id %-3d rec %-3d %-28s %-10s %s" % (level_id, record, name, source, ok))
        log.append({"id": level_id, "record": record, "derived_name": name,
                    "source": source, "reached": reached,
                    "image": str(path)})

    (out / "sweep.json").write_text(json.dumps(log, indent=2), encoding="utf-8")
    print("\nwrote %d captures and %s" % (len(log), out / "sweep.json"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
