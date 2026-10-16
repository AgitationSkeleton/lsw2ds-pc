"""Can each registered dispatch override actually be reached?

WHY THIS EXISTS. The fork's save-gate waiver was once found to have never
fired across three live test runs, and `dispatch_overrides.cpp` said so in its
own comment without knowing why. The reason was not the test and not the seam:

    The recompiler emits a DIRECT C CALL for a branch whose target lands in the
    same generated bank. Only cross-bank and indirect transfers go through
    `runtime_dispatch`, and an override lives in the dispatch table.

`0x02060084` and `0x020600F0` - the two state-message enqueues, byte-identical
twins - sit in `lsw2_arm9_17.c` together with their callers, so the generated
code calls `lsw2_arm9_FUN_02060084()` by symbol. Registering an override for
that address is legal, silent, and completely inert.

That is the worst failure shape this project produces: code that exists, is
registered, has a test, and cannot run. Nothing in the build says so.

    python tools/sweep_override_reachable.py

WHAT IT CHECKS. For every address in `kOverridesCore` / `kOverridesWireless`, it
counts direct call sites of the corresponding lifted symbol in the generated
sources. A direct call site is a call the override does NOT intercept.

  * zero direct call sites  -> every call dispatches; the override is reached
  * some direct call sites  -> those callers bypass it; reported

WHAT IT CANNOT DO. It cannot tell whether a bypassed override still does
something useful through its remaining dispatched callers - only that some
callers skip it. An address with direct callers is listed with the count so the
reader can judge; only an override with NO dispatched route at all is a hard
failure, because that one provably cannot run.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "port" / "frontend" / "dispatch_overrides.cpp"
GEN = ROOT / "out" / "work" / "ndsrecomp" / "generated"

# Bypasses that were checked and are harmless, with the reason. A bypass only
# MATTERS when the override changes behaviour; one that reproduces the original
# exactly gives its bypassing callers the same answer either way.
#
# Keep this list short and each entry justified by a reading of both bodies -
# it is an assertion that the two agree, not a way to silence the gate.
BENIGN = {
    0x020063D4: (
        "the one bypassing caller is 0x02006220, the stock-appearance applier "
        "(BL at 0x0200625C), which applies a character's OWN hat - the case "
        "where the covering path is already correct, so the head fix's correction is "
        "not wanted there."),
    0x020AD310: (
        "wh_get_system_state reproduces the original exactly - both return "
        "[0x0210B320 + 0x20] and nothing else (0x020AD310: LDR r0,=0x0210B320 / "
        "LDR r0,[r0,#0x20] / BX lr), so the two callers in 0x020AF594 that call "
        "it by symbol get the same value. Only the log line undercounts."),
}

ENTRY = re.compile(r"\{\s*(0x[0-9A-Fa-f]{8})u\s*,\s*\d+\s*,\s*(\w+)\s*,")
TABLE = re.compile(r"const NdsDispatchEntry kOverrides\w+\[\]\s*=\s*\{(.*?)\n\};", re.S)


def main() -> int:
    if not SRC.is_file():
        print(f"no {SRC}")
        return 1
    text = SRC.read_text(encoding="utf-8", errors="replace")
    entries = []
    for block in TABLE.finditer(text):
        for m in ENTRY.finditer(block.group(1)):
            entries.append((int(m.group(1), 16), m.group(2)))
    if not entries:
        print("no override tables found - has dispatch_overrides.cpp changed shape?")
        return 1

    if not GEN.is_dir():
        print(f"generated sources not present at {GEN}")
        print("  (run the lift first; this gate needs them and is skipped)")
        return 0

    bodies = list(GEN.glob("lsw2_arm9_*.c"))
    if not bodies:
        print("no generated ARM9 banks; skipped")
        return 0
    blob = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                     for p in bodies if not p.name.endswith("dispatch.c"))
    dispatch = (GEN / "lsw2_arm9_dispatch.c")
    dtext = dispatch.read_text(encoding="utf-8", errors="replace") if dispatch.is_file() else ""

    bad, warned, ok = [], [], 0
    for addr, name in entries:
        sym = f"lsw2_arm9_FUN_{addr:08x}"
        direct = blob.count(f"{sym}();")
        # A call the override DOES intercept exists only if the address is in
        # the dispatch table at all.
        dispatched = f"{{0x{addr:08X}u," in dtext
        if not dispatched:
            bad.append(f"{name} at 0x{addr:08X}: not in the dispatch table - "
                       f"it can never be reached")
        elif direct and addr in BENIGN:
            ok += 1
        elif direct:
            warned.append(f"{name} at 0x{addr:08X}: {direct} direct call site(s) "
                          f"bypass it ({sym} called by symbol)")
            ok += 1
        else:
            ok += 1

    for line in warned:
        print(f"  bypassed: {line}")
    if bad:
        print("overrides that cannot run:")
        for line in bad:
            print(f"  {line}")
        return 1

    print(f"{ok} registered overrides are in the dispatch table"
          f"{f'; {len(warned)} also have direct callers that bypass them' if warned else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
