"""Every guest address the port calls or overrides must be a function ENTRY.

WHY THIS EXISTS. The port names 233 distinct guest addresses. Most are tables,
RAM, or instruction citations in comments, but a few are used as CALL TARGETS -
override table rows and `lsw2_guest_call` arguments - and those must be entries
the recompiler knows about. An address that is one instruction into a function,
or inside a literal pool, produces a call that lands nowhere, and the failure is
a crash or silence a long way from the mistake.

`armscan.entry_of` cannot answer this: it walks back to the nearest prologue and
LEAF FUNCTIONS HAVE NONE. `0x02006360` (three instructions, `STRB`/`BX lr`),
`0x02086204`, `0x0208DA74`, `0x020AD310` and `0x02060158` are all real
functions that `entry_of` attributes to a neighbour - so an audit built on it
reports false alarms and hides real ones behind the noise.

THE AUTHORITY IS THE RECOMPILER. Its generated dispatch table names every
function it lifted, and each row maps a dispatchable address to the symbol
whose entry it belongs to. A row whose address equals its symbol's address IS
an entry, by the recompiler's own reckoning.

    python tools/sweep_guest_addresses.py

WHAT IT DOES NOT CHECK. Whether the target is the RIGHT function - only that it
is a function. `tools/sweep_override_reachable.py` answers whether an override
there can run, and `tools/sweep_override_targets.py` asks whether the
replacement behaves like the original.
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "out" / "work" / "ndsrecomp" / "generated"
FRONTEND = ROOT / "port" / "frontend"

ROW = re.compile(r"\{0x([0-9A-F]{8})u,\s*\d+u,\s*lsw2_arm9_FUN_([0-9a-f]{8}),")
OVERRIDE = re.compile(r"\{\s*(0x[0-9A-Fa-f]{8})u\s*,\s*\d+\s*,\s*(\w+)")
GUESTCALL = re.compile(r"lsw2_guest_call\(\s*lsw2_arm9_FUN_([0-9a-f]{8})")


def main() -> int:
    dispatch = GEN / "lsw2_arm9_dispatch.c"
    if not dispatch.is_file():
        print(f"skipped: {dispatch} not present (run the lift first)")
        return 0

    entries = {int(sym, 16)
               for _, sym in ROW.findall(
                   dispatch.read_text(encoding="utf-8", errors="replace"))}
    if not entries:
        print("no function entries parsed - has the dispatch table changed shape?")
        return 1

    targets = []
    src = (FRONTEND / "dispatch_overrides.cpp").read_text(
        encoding="utf-8", errors="replace")
    for addr, name in OVERRIDE.findall(src):
        targets.append((int(addr, 16), f"override {name}"))

    for path in sorted(glob.glob(str(FRONTEND / "*.cpp"))):
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        for sym in GUESTCALL.findall(text):
            targets.append((int(sym, 16), f"guest call in {Path(path).name}"))

    bad = [(a, why) for a, why in targets if a not in entries]
    if bad:
        print("guest addresses used as call targets that are NOT function entries:")
        for a, why in bad:
            print(f"  0x{a:08X}  ({why})")
        print("\n  A call to a non-entry lands mid-function or in a literal pool.")
        return 1

    print(f"all {len(targets)} guest call targets are function entries "
          f"the recompiler knows ({len(entries)} known)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
