"""Features required in BOTH builds must not sit in a fork-only table.

WHY THIS EXISTS. `lsw2_register_dispatch_overrides` installs `kOverridesCore`
only when `core` is true, and outside `LSW2_HLE=1` that means only in the fork:

    const bool core = on ? (*on == '1') : fork;
    ...
    if (!core) return;

Three things have now been registered in the wrong place for exactly that
reason, and the failure is silent every time - the code is correct, reachable,
tested, and watching a build in which its subject does not exist:

  * `statelog.h`   caught AFTER shipping. The states it exists to name are
                   reached through the Cantina console and the pause wireless
                   row, and the fork warps past one and hides the other.
  * `statereq.h`   caught BEFORE shipping, same reasoning.
  * the head fix   caught by audit. Its applier is an enhancement setting for
                   both the base port and the Bounty Hunter fork, and the
                   baseline's customize panel applies hats through the very
                   function the override sits on.

    python tools/sweep_build_gating.py

WHAT IT CHECKS. Each entry below names an override that must reach the baseline
and asserts it appears in `kOverridesAlways` rather than `kOverridesCore`. The
list is short and every entry cites why the baseline needs it - a feature nobody
registered here is exactly as unchecked as before, which is the same limitation
`sweep_rom_constants.py` carries and for the same reason.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "port" / "frontend" / "dispatch_overrides.cpp"

# (address, why the BASELINE needs it)
MUST_REACH_BASELINE = [
    (0x0205ECC4, "names the state AND requester for the master server's co-op "
                 "listing; the Cantina "
                 "console and the pause wireless row exist only in the baseline"),
    (0x02086204, "Free Play portrait - the Free Play widget pointer is captured here "
                 "when lr names the constructor, and the portrait fix is an "
                 "Enhancement in both builds; the fork-only forcing in the same "
                 "function is unaffected because it tests fork state"),
    (0x020063D4, "head fix - an enhancement setting for both the base port "
                 "and the Bounty Hunter fork; the baseline's customize "
                 "panel applies hats through this function"),
]

TABLE = re.compile(r"const NdsDispatchEntry (kOverrides\w+)\[\]\s*=\s*\{(.*?)\n\};",
                   re.S)
ROW = re.compile(r"\{\s*(0x[0-9A-Fa-f]{8})u\s*,")


def main() -> int:
    if not SRC.is_file():
        print(f"no {SRC}")
        return 1
    text = SRC.read_text(encoding="utf-8", errors="replace")

    where = {}
    for m in TABLE.finditer(text):
        for addr in ROW.findall(m.group(2)):
            where.setdefault(int(addr, 16), []).append(m.group(1))
    if not where:
        print("no override tables found - has the file been restructured?")
        return 1

    bad = []

    # A TABLE REGISTERED BY A FUNCTION CALL, which the table scan above cannot
    # see at all. `kCoopOverrides` lives in coopjoin.cpp and reaches the
    # dispatcher through `lsw2_coopjoin_install()`, so no amount of reading
    # `kOverrides*` here will find it - and it was installed on the WRONG SIDE
    # of `if (!core) return;` for its whole existence.
    #
    # The effect was the reverse of the override's own comment in both
    # directions: the baseline, which needs it for the cartridge's own co-op
    # lobby list, never installed it; the fork, whose comment says a second list "would
    # put a second, different list in front of the same player", did. The
    # `if (ui_fork_build()) return;` meant to stop that half is inert, because
    # ui_fork_build() is false at registration time in both executables.
    #
    # So the check is POSITIONAL: the call must appear before the fork-only
    # return, which is the same thing the kOverridesAlways rule asserts for the
    # tables, expressed for a call site.
    call = text.find("lsw2_coopjoin_install(")
    gate = text.find("if (!core) return;")
    if call < 0:
        bad.append("lsw2_coopjoin_install() is not called at all\n"
                   "      needed: the cartridge's own co-op lobby list - the "
                   "baseline's lobby list joins over TCP")
    elif gate < 0:
        bad.append("the `if (!core) return;` fork gate has gone - this check "
                   "cannot tell which side the co-op install is on")
    elif call > gate:
        bad.append("lsw2_coopjoin_install() is called AFTER `if (!core) "
                   "return;`\n      so it installs in the fork and never in "
                   "the baseline - the reverse of its contract on both sides")

    for addr, why in MUST_REACH_BASELINE:
        tables = where.get(addr)
        if not tables:
            bad.append(f"0x{addr:08X} is not registered at all\n      needed: {why}")
        elif "kOverridesAlways" not in tables:
            bad.append(f"0x{addr:08X} is in {', '.join(tables)}, which the "
                       f"baseline does not install\n      needed: {why}")

    if bad:
        print("overrides the baseline needs but will not get:")
        for line in bad:
            print(f"  {line}")
        print("\n  kOverridesCore installs only when `core` is true, which "
              "outside\n  LSW2_HLE=1 means only in the fork.")
        return 1

    print(f"all {len(MUST_REACH_BASELINE)} baseline-critical overrides are in "
          f"kOverridesAlways, and the co-op install is on the baseline side "
          f"of the fork gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
