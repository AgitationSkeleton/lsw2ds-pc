#!/usr/bin/env python3
"""Sample memory addresses from the running emulator at high rate, to CSV.

Physics cannot be fitted from the session recording: it samples every ten
frames, and a jump arc is over in about thirty. Raising the recording rate
would mean restarting the emulator and losing the match. Reading the live
process instead needs no restart and no cooperation from the game - the
emulator is not paused and does not know it is being watched.

Main RAM is located the same way `read_process_memory.py` does it, by matching
two distant ARM9 signatures so a chance hit cannot mislocate the window.

Values are written as signed 20.12 fixed point by default, which is how the
engine stores world coordinates and velocities: the raw integer
is divided by 4096 to give world units. Pass --raw to skip that.

Run:
    python tools/poll_addresses.py --addr 0x0210a260 0x0210a268 --seconds 20
    python tools/poll_addresses.py --addr 0x0210a260 --hz 120 --raw
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_process_memory import (  # noqa: E402
    Process, find_melonds_pid, locate_main_ram, ARM9,
)

ROOT = Path(__file__).resolve().parents[1]
FIXED_SHIFT = 12


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--addr", nargs="+", required=True,
                        help="DS addresses, e.g. 0x0210a260")
    parser.add_argument("--width", type=int, default=4, choices=[2, 4])
    parser.add_argument("--hz", type=float, default=60.0)
    parser.add_argument("--seconds", type=float, default=15.0)
    parser.add_argument("--raw", action="store_true",
                        help="report raw integers instead of 20.12 fixed point")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/poll.csv")
    arguments = parser.parse_args()

    addresses = [int(a, 0) for a in arguments.addr]
    pid = arguments.pid or find_melonds_pid()
    process = Process(pid)
    try:
        regions = process.regions()
        arm9 = ARM9.read_bytes()
        base, offset = locate_main_ram(process, regions, arm9)
        if base is None:
            raise SystemExit("could not locate main RAM")
        origin = base + offset
        print(f"pid {pid}: main RAM at {origin:#018x}")
        print(f"sampling {len(addresses)} addresses at {arguments.hz} Hz "
              f"for {arguments.seconds}s")

        # One contiguous read per sample keeps the addresses coherent with each
        # other; reading them separately would smear a moving value across
        # different frames.
        lowest = min(addresses)
        highest = max(addresses) + arguments.width
        span = highest - lowest
        fmt = "<i" if arguments.width == 4 else "<h"

        rows = []
        interval = 1.0 / arguments.hz
        deadline = time.perf_counter() + arguments.seconds
        next_at = time.perf_counter()
        while time.perf_counter() < deadline:
            now = time.perf_counter()
            if now < next_at:
                continue
            next_at += interval
            block = process.read(origin + (lowest - 0x02000000), span)
            if not block or len(block) < span:
                continue
            values = [struct.unpack_from(fmt, block, a - lowest)[0]
                      for a in addresses]
            rows.append((now, values))

        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        with arguments.output.open("w", encoding="utf-8") as handle:
            handle.write("t," + ",".join(f"{a:#010x}" for a in addresses) + "\n")
            start = rows[0][0] if rows else 0.0
            for when, values in rows:
                if arguments.raw:
                    text = ",".join(str(v) for v in values)
                else:
                    text = ",".join(f"{v / (1 << FIXED_SHIFT):.5f}" for v in values)
                handle.write(f"{when - start:.4f},{text}\n")

        print(f"{len(rows)} samples -> {arguments.output}")
        if rows:
            changed = [
                f"{a:#010x}" for i, a in enumerate(addresses)
                if len({r[1][i] for r in rows}) > 1
            ]
            print(f"addresses that changed: {', '.join(changed) or 'none'}")
    finally:
        process.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
