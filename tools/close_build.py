"""Close the build over what the code ACTUALLY calls, not what the inventory counted.

WHY THE INVENTORY UNDERCOUNTS. The closure in config/recomp_inventory.json was
built from Ghidra's recorded call edges. Ghidra records a call it resolved to a
function as `FUN_xxxxxxxx`, and a call it resolved to an ADDRESS WITH NO
FUNCTION as `func_0xxxxxxxxx`. Only the first kind became an edge.

So the inventory's 1,555 is a floor, not the closure. The link proves it:
FUN_01ff9f20 calls FUN_02055530, which is not in the file list and therefore was
never compiled, and the same for dozens of others.

This walks the ACTUAL references. Every undefined symbol that names a function
with a decompiled body is added to the build and the sweep runs again, until
nothing new appears. What is left after that is genuinely absent - not merely
uncounted - and gets an aborting stub.

The number this produces replaces the inventory's as the denominator, because it
is the one the linker agrees with.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORK = ROOT / "out" / "work" / "closure_build"
DIRS = [ROOT / "out" / "ghidra" / "decomp" / "c",
        ROOT / "out" / "ghidra" / "decomp_itcm" / "c"]

NAME = re.compile(r"^(?:thunk_)?FUN_([0-9a-fA-F]{8})$")


def source_for(name: str):
    for d in DIRS:
        p = d / (name + ".c")
        if p.exists():
            return p
    # A thunk_ name is Ghidra's label for a veneer; its body lives in the file
    # named for the veneer's own address, which the caller cannot know. Those
    # are handled by the host layer, not here.
    return None


def main() -> int:
    files = [l.strip() for l in
             (WORK / "files.txt").read_text(encoding="utf-8").splitlines()
             if l.strip()]
    have = {pathlib.Path(f).stem for f in files}

    missing_path = WORK / "missing.txt"
    if not missing_path.exists():
        print("no missing.txt yet - run the link step first")
        return 1
    missing = [l.strip() for l in
               missing_path.read_text(encoding="utf-8").splitlines() if l.strip()]

    added = []
    for name in missing:
        if not NAME.match(name) or name in have:
            continue
        p = source_for(name)
        if p:
            added.append(str(p.relative_to(ROOT)).replace("\\", "/"))
            have.add(name)

    if added:
        with (WORK / "files.txt").open("a", encoding="utf-8") as f:
            for a in added:
                f.write(a + "\n")

    print("functions already in the build   %d" % len(files))
    print("added from actual references     %d" % len(added))
    print("still absent (will be stubbed)   %d"
          % len([n for n in missing
                 if NAME.match(n) and not source_for(n)]))
    # Exit 10 means "there was more to add" so the build loop knows to iterate.
    return 10 if added else 0


if __name__ == "__main__":
    sys.exit(main())
