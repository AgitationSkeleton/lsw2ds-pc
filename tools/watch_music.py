"""Log every music change, with the level it happened in.

WHY THIS AND NOT AN OVERRIDE. An earlier plan had this as a dispatch override on
`0x020B90E4`. That is wrong, and the reason is worth stating: the dispatch seam
REPLACES a function, it does not wrap one. Logging from an override would mean
reimplementing the request function - thirty-odd instructions with real side
effects - and any mistake in that would change the music rather than observe it.

Watching costs nothing and cannot change anything. `0x0210DB14 + 9` is the slot
the game currently has playing - the request function reads it at `0x020B9108`
to decide whether a request is a no-op - so polling that byte sees every change
the game makes, whoever asked for it.

    python tools/watch_music.py

Then play. Every music change prints a line with the level index. A level that
switches from a calm track to a battle one prints its own pair, in order, with
no guessing about which of the 29 slots is which - which is exactly the evidence
the host menu's `Music: RANDOM` option needs and cannot get from the ROM.

Run it against an ordinary interactive session; it needs no special flags. The
port's debug surface is always up in interactive mode.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

HOST = "127.0.0.1"
PORT = 19842

# The music context. Resolved from the request function's own literal pool -
# both 0x020B90E8 and 0x020B9100 load it from 0x020B917C.
MUSIC_CTX = 0x0210DB14
SLOT_PLAYING = 9        # ldrb r1,[r0,#9]  - the slot currently playing
FLAGS = 0xA             # the next byte: bit 0x20 suppresses music entirely

# The level, by the state-manager chain, verified two ways.
STATE_MANAGER = 0x02102900
LEVEL_INDEX = 0x0C


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

    def u32(self, addr):
        return struct.unpack("<I", bytes.fromhex(
            self.command(cmd="read_mem", addr=addr, len=4)["hex"]))[0]

    def bytes_at(self, addr, n):
        return bytes.fromhex(self.command(cmd="read_mem", addr=addr, len=n)["hex"])


def level_of(s: Surface) -> int:
    """-1 when the game is not in a level - a menu or the boot."""
    ctx = s.u32(STATE_MANAGER)
    if not (0x02000000 <= ctx < 0x02400000):
        return -1
    return s.u32(ctx + LEVEL_INDEX)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="also append the log to this file")
    ap.add_argument("--interval", type=float, default=0.1,
                    help="seconds between samples (default 0.1)")
    args = ap.parse_args()

    try:
        s = Surface()
    except OSError as exc:
        print("cannot reach the port on %s:%d - %s" % (HOST, PORT, exc),
              file=sys.stderr)
        print("Start the game first; interactive mode always serves this.",
              file=sys.stderr)
        return 1

    out = open(args.out, "a", encoding="utf-8") if args.out else None
    print("watching %08X+%d - play normally, Ctrl-C to stop" %
          (MUSIC_CTX, SLOT_PLAYING))
    print("%8s  %5s  %4s  %s" % ("time", "level", "slot", "note"))

    t0 = time.time()
    last_slot = None
    last_level = None
    try:
        while True:
            blob = s.bytes_at(MUSIC_CTX + SLOT_PLAYING, 2)
            slot, flags = blob[0], blob[1]
            level = level_of(s)

            if slot != last_slot or level != last_level:
                note = ""
                if last_slot is not None and level == last_level:
                    # SAME LEVEL, DIFFERENT SLOT. This is the transition the
                    # whole exercise is for - a calm/battle pair naming itself.
                    note = "<- switch within the level (slot %d -> %d)" % (
                        last_slot, slot)
                elif level != last_level:
                    note = "level change"
                if flags & 0x20:
                    note += "  [music suppressed]"
                line = "%7.1fs  %5s  %4d  %s" % (
                    time.time() - t0,
                    level if level >= 0 else "-", slot, note)
                print(line)
                if out:
                    out.write(line + "\n")
                    out.flush()
                last_slot, last_level = slot, level

            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        if out:
            out.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
