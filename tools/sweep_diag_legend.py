"""Every line LSW2_DIAG prints must be explained in the legend it prints first.

WHY THIS EXISTS. `lsw2_diag_announce()` prints a legend - "what each line
answers" - and `lsw2_diag_tick()` prints the lines. They are two lists in one
file that have to agree, and they drifted: `netplayers` and `netseq` were being
printed with no legend entry at all, and `netplayers` is the line the ledger
calls **the biggest one** - the byte that gates the cartridge's whole message
layer.

That is worse than an ordinary documentation gap. The live test runs these
diagnostics exist for are expensive and infrequent, and a reader who works from
the legend does not know to look for a line the legend never mentions. An
unexplained line in a wall of output is an unasked question.

The reverse - a legend entry for a line that is never printed - is the same
failure wearing the other face: it sends the reader looking for output that
cannot appear, and they conclude the feature is broken rather than the legend.

    python tools/sweep_diag_legend.py

Exit 1 on any mismatch, so it can be a gate.

WHAT IT DOES NOT CHECK. Whether the explanation is any good. It matches names,
not meanings.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "port" / "frontend" / "diagnostics.cpp"

# Lines the tick prints through something other than say(). Listed rather than
# pattern-matched, so a new one is a deliberate addition here and not an
# accidental exemption.
PRINTED_BY_HAND = {
    "level",   # printed with its name from levels.h, so it is not a say()
    "slot",    # "slot %d  char ... parts ..." - one line per station
    "round",   # a long rather than an int, so it cannot go through say()
}


def main() -> int:
    text = SRC.read_text(encoding="utf-8", errors="replace")

    # The legend is everything inside lsw2_diag_announce().
    m = re.search(r"void lsw2_diag_announce\(\)(.*?)\n\}", text, re.S)
    if not m:
        print("could not find lsw2_diag_announce() - has it been renamed?")
        return 1
    legend = m.group(1)

    # A legend entry is `"[diag]   name   description`: three spaces, the name,
    # then AT LEAST TWO more spaces to reach the description column.
    #
    # The two-space tail is what separates an entry from prose. Matching on the
    # leading three spaces alone also matched continuation sentences that happen
    # to start at that column - "then walk up to the console", "For <feature>: ..." -
    # and reported "then", "the", "found" and "overrides" as legend entries for
    # lines that are never printed. A check that cries wolf gets switched off.
    explained = set(re.findall(r'"\[diag\]   ([a-z_]+)(?: N)?  +', legend))

    printed = set(re.findall(r'say\("([a-z_]+)"', text)) | PRINTED_BY_HAND

    missing = sorted(printed - explained)
    stale = sorted(explained - printed)

    if missing:
        print("PRINTED BUT NOT EXPLAINED - a reader will not know to look:")
        for n in missing:
            print(f"  {n}")
    if stale:
        print("EXPLAINED BUT NEVER PRINTED - a reader will look for nothing:")
        for n in stale:
            print(f"  {n}")

    if missing or stale:
        return 1

    print(f"legend and tick agree - {len(printed)} lines, all explained")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
