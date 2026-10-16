"""Every dispatch override REPLACES a function. Who else called it?

WHY THIS EXISTS. An override does not wrap or defer - it replaces.
So the moment the port registers one, it has taken responsibility for every
caller that function ever had, not just the one the design was about.

That is easy to forget, and forgetting it once cost three passes in a row:

  - The first proposed overriding 0x020B111C because the pause menu's wireless
    row calls it. It has EIGHT callers in four functions.
  - The second kept the idea behind an `lr` gate. That does not help either, because
    intercepting one call does not stop what the caller does next.
  - The third settled it by counting callers on the whole path first - 8, 14 and
    176 - which took one scan and should have come before either design.

None of the four test gates could catch that, because the code was never
written: it was a plan, and plans are where this kind of error lives.

WHAT THIS DOES. Reads the addresses out of dispatch_overrides.cpp's
NdsDispatchEntry tables, counts each one's callers in the ROM, and compares
against a baseline. A NEW override, or one whose caller count MOVED, fails.

    python tools/sweep_override_targets.py
    python tools/sweep_override_targets.py --update    rewrite the baseline

WHAT A HIGH COUNT MEANS - and does not. It is not a failure. `0x02086204` is a
generic flag test with many callers and the port reimplements it exactly, which
is correct. The number is a QUESTION: does this override behave like the
original for all of them? Answering "yes, deliberately" is fine. Never having
asked is the problem.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armscan import load, callers, word_refs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "port" / "frontend" / "dispatch_overrides.cpp"
BASELINE = ROOT / "config" / "override_targets.json"

ENTRY = re.compile(r"\{\s*(0x0[0-9A-Fa-f]{7})u\s*,\s*\d+\s*,\s*(\w+)")

# AN ENTRY MAY NAME ITS ADDRESS INSTEAD OF SPELLING IT. For example,
# commguard's override was registered as `{ kLsw2NetConsistencyCheck, 0, ... }`
# - a name, because sweep_duplicate_constants forbids a second literal for a
# value a header already owns - and this gate, which matched hex literals only,
# reported "all 23 override targets match the baseline" while 24 were
# registered. One gate's rule had opened a hole in the other. A NEW override is
# exactly the thing this gate exists to stop, so a name must resolve to its
# value and be counted like a literal.
NAMED_ENTRY = re.compile(r"\{\s*(k[A-Za-z0-9_]+)\s*,\s*\d+\s*,\s*(\w+)")
CONST_DEF = re.compile(
    r"constexpr\s+uint32_t\s+(k[A-Za-z0-9_]+)\s*=\s*(0x0[0-9A-Fa-f]{7})u\s*;")


def named_constants() -> dict[str, int]:
    """Every `constexpr uint32_t kName = 0x........u;` in the frontend headers
    and sources, so a table entry written by name can be resolved."""
    out: dict[str, int] = {}
    for path in sorted((ROOT / "port" / "frontend").glob("*.h")) + \
                sorted((ROOT / "port" / "frontend").glob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in CONST_DEF.finditer(text):
            out.setdefault(m.group(1), int(m.group(2), 16))
    return out


def registered() -> list[tuple[int, str]]:
    """(address, handler name) for every NdsDispatchEntry in the port."""
    text = SRC.read_text(encoding="utf-8", errors="replace")
    # Strip comments so a commented-out entry is not counted as registered.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    found = [(int(m.group(1), 16), m.group(2)) for m in ENTRY.finditer(text)]
    consts = named_constants()
    for m in NAMED_ENTRY.finditer(text):
        name, handler = m.group(1), m.group(2)
        if name in consts:
            found.append((consts[name], handler))
        else:
            # Refusing to guess: an entry the gate cannot resolve is reported,
            # not silently skipped - silently skipping is how this hole opened.
            print(f"cannot resolve named override target {name} ({handler}) - "
                  f"add its constexpr uint32_t definition or spell the literal")
            sys.exit(1)
    return found


def main(argv: list[str]) -> int:
    update = "--update" in argv
    data = load()
    entries = registered()
    if not entries:
        print("no NdsDispatchEntry rows found - has the table moved?")
        return 1

    counts = {}
    for addr, name in entries:
        n = len(callers(data, addr)) + len(word_refs(data, addr))
        counts[f"0x{addr:08X}"] = {"handler": name, "callers": n}

    if update or not BASELINE.is_file():
        BASELINE.write_text(
            json.dumps({"schema": 1, "note": __doc__, "targets": counts},
                       indent=1) + "\n",
            encoding="utf-8")
        print(f"wrote baseline for {len(counts)} override targets")
        for k, v in sorted(counts.items()):
            print(f"   {k}  {v['callers']:4d} callers  {v['handler']}")
        return 0

    base = json.loads(BASELINE.read_text(encoding="utf-8"))["targets"]
    bad = []
    for key, v in sorted(counts.items()):
        if key not in base:
            bad.append(f"{key} ({v['handler']}) is a NEW override with "
                       f"{v['callers']} caller(s) - does it behave like the "
                       f"original for all of them?")
        elif base[key]["callers"] != v["callers"]:
            bad.append(f"{key} ({v['handler']}) had {base[key]['callers']} "
                       f"callers, now {v['callers']} - the ROM reading moved")
    for key in sorted(base):
        if key not in counts:
            bad.append(f"{key} is baselined but no longer registered - "
                       f"remove it from the baseline")

    if bad:
        print("override targets need a decision:")
        for line in bad:
            print(f"  {line}")
        print("\n  If the change is intended, re-run with --update.")
        return 1

    total = sum(v["callers"] for v in counts.values())
    print(f"all {len(counts)} override targets match the baseline "
          f"({total} callers between them)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
