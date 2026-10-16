"""Every `lr ==` constant must name a site that CALLS a function we override.

WHY THIS EXISTS. An override identifies its caller by comparing the guest's link
register against a return address. That works only when the overridden function
is called DIRECTLY from the site being recognised - and one of these was
once wrong in a way that survived two rounds of live testing:

    0208E9C8  BL 0x02066708        the pause menu's row loop   lr = 0x0208E9CC
      02066708  STM sp!, {r3, lr}  a wrapper
      02066714  BL 0x0205CD20      <- the override is HERE     lr = 0x02066718

The override sits on `0x0205CD20`, two frames below the loop, so `lr` was always
`0x02066718` and never `0x0208E9CC`. The condition could not fire. Every pause
menu fix written on top of it - the wireless icon, the hidden-row cursor, the
row layout - therefore never executed, and the same four symptoms came back
unchanged on the next test.

Nothing about that was visible in C. The build was green, the tests passed, the
constant was correct, the disassembly it came from was correct. Only the
RELATIONSHIP between them was wrong.

    python tools/sweep_lr_checks.py

For each `... = 0xADDR;` constant whose name ends in Return and which is compared
against `g_cpu.R[14]`, decode the word at ADDR-4 and require it to be a BL to one
of the addresses in the override tables. Exit 1 on any that is not.

WHAT IT CANNOT CHECK. Whether the constant is the right site - only whether the
site calls something we actually replace. A `lr` pointing at a direct call to the
wrong overridden function still passes.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "port" / "frontend" / "dispatch_overrides.cpp"
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000


def bl_target(data: bytes, addr: int):
    """The target of the BL at `addr`, or None if that word is not a BL."""
    off = addr - BASE
    if off < 0 or off + 4 > len(data):
        return None
    word = struct.unpack_from("<I", data, off)[0]
    if (word >> 24) & 0x0F != 0x0B:
        return None
    imm = word & 0xFFFFFF
    if imm & 0x800000:
        imm -= 0x1000000
    return addr + 8 + (imm << 2)


GEN = ROOT / "out" / "work" / "ndsrecomp" / "generated"


def generated_text() -> str:
    """Every generated ARM9 bank body, or "" when the lift is not present."""
    if not GEN.is_dir():
        return ""
    return "\n".join(
        p.read_text(encoding="utf-8", errors="replace")
        for p in sorted(GEN.glob("lsw2_arm9_*.c"))
        if not p.name.endswith("dispatch.c"))


_GENERATED = None


def dispatches(ret: int, target: int) -> bool:
    """Does the call that returns to `ret` go through the dispatch table?

    WHY THIS MATTERS AND WHY IT IS NOT THE SAME QUESTION AS THE ONE ABOVE.
    An override lives in the dispatch table, and the recompiler consults that
    table only for cross-bank and indirect transfers. A branch whose target
    lands in the SAME generated bank is emitted as a plain C call by symbol -
    `lsw2_arm9_FUN_02060084();` - which the override never sees.

    On 2026-08-29 two enqueue overrides were found to have been registered,
    tested and inert for exactly that reason, and a source comment had drawn a
    conclusion from the silence of a trace that could not run. An `lr` check
    sitting under such a call is doubly dead: the site is right, the override is
    right, and neither ever executes.

    The generated code names the site directly, so this is a lookup rather than
    a bank calculation: find the call site that sets R[14] to this return
    address, and see whether the call that follows is a direct symbol call.
    """
    global _GENERATED
    if _GENERATED is None:
        _GENERATED = generated_text()
    if not _GENERATED:
        return True                      # nothing lifted; cannot judge, so pass
    marker = f"g_cpu.R[14] = 0x{ret:08X}u;"
    i = _GENERATED.find(marker)
    if i < 0:
        return True                      # site not in the lift; leave it alone
    window = _GENERATED[i:i + 1200]
    return f"lsw2_arm9_FUN_{target:08x}();" not in window


def main() -> int:
    if not IMAGE.exists():
        print(f"skipped: {IMAGE} is not extracted (run the ROM extraction first)")
        return 0

    text = SRC.read_text(encoding="utf-8", errors="replace")
    data = IMAGE.read_bytes()

    # The functions we replace, from every NdsDispatchEntry table in the file.
    overridden = {
        int(m, 16)
        for m in re.findall(r"\{\s*(0x[0-9A-Fa-f]{8})u?\s*,\s*\d+\s*,", text)
    }
    if not overridden:
        print("no override tables found - has the file been restructured?")
        return 1

    # Constants actually compared against the link register.
    compared = set(re.findall(r"R\[14\]\s*==\s*(k[A-Za-z0-9_]+)", text))
    if not compared:
        print("no lr comparisons found; nothing to check")
        return 0

    bad = []
    for name in sorted(compared):
        m = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+)", text)
        if not m:
            bad.append((name, "no constant found"))
            continue
        ret = int(m.group(1), 16)
        target = bl_target(data, ret - 4)
        if target is None:
            bad.append((name, f"{ret - 4:#010x} is not a BL"))
        elif target not in overridden:
            bad.append((name, f"calls {target:#010x}, which is NOT overridden"))
        elif not dispatches(ret, target):
            bad.append((name, f"calls {target:#010x}, which IS overridden - but "
                              f"the recompiler emits that call DIRECTLY "
                              f"(same generated bank), so the override never "
                              f"runs for this site"))

    if bad:
        print("lr checks that can never fire:")
        for name, why in bad:
            print(f"  {name}: {why}")
        return 1

    print(f"all {len(compared)} lr checks sit under a direct call to an "
          f"override that the dispatch table actually reaches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
