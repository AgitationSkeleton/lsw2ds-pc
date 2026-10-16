#!/usr/bin/env python3
"""Find SEAMS: functions that join two proven subsystems and are tested by none.

WHY. Every client joining this fork over TCP was once found to seat itself
on station -1. `test_master_client.cpp` covers the directory client;
`lsw2_lockstep`'s tests run four stations over a fake transport. Both halves
were green. The bug lived in `join_address`, the function where they meet - and
no test calls it.

That is a shape, not an accident:

    subsystem A   tested
    subsystem B   tested
    A -> B glue   called by nothing but the running game

A unit test on either half is evidence about that half and none about the join,
which is rule (a) seen from the other side.

WHAT THIS REPORTS. Every function defined in port/frontend and port/net that:

  1. no test_*.cpp names, and
  2. references symbols from more than one subsystem

ranked by how many subsystems it touches, because a function bridging three is a
better place to look than one bridging two. This is a SEARCH, not a verdict:
plenty of untested glue is fine, and the tool cannot tell which. It narrows
where to read.

    python tools/sweep_untested_seams.py [--top N]
"""

from __future__ import annotations

import argparse
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port/frontend", ROOT / "port/net"]

# A subsystem is a header family. A function that touches several of these is
# doing integration work, which is exactly what unit tests tend to skip.
SUBSYSTEMS = {
    "transport": ["Lsw2TcpTransport", "transport_", "lsw2_tcp", "->join(", "->host("],
    "lockstep": ["Lsw2LockstepSession", "lockstep_", "lsw2_net_set_active_session"],
    "directory": ["master_", "Lsw2MasterClient", "lsw2_master"],
    "guest": ["bus_device_read32", "bus_device_write32", "g_cpu", "lsw2_arm9_"],
    "state": ["lsw2_state_", "kLsw2StateManager", "lsw2_cheats_", "statereq"],
    "menu": ["Lsw2Menu", "menu_", "rebuild_menu", "lsw2_menuhost"],
    "wireless": ["lsw2_wh_", "kLsw2Wireless", "WH_"],
    "settings": ["settings_", "g_store", "lsw2_ui_"],
    "render": ["present", "lsw2_gx", "ImGui::"],
}

FUNC = re.compile(
    r"^(?:[A-Za-z_][\w:<>,\s\*&]*?)\b"
    r"(?:(Lsw2\w+)::)?([a-zA-Z_]\w*)\s*\([^;]*\)\s*(?:const\s*)?\{",
    re.MULTILINE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    tests = []
    for d in DIRS:
        for p in d.glob("test_*.cpp"):
            tests.append(p.read_text(encoding="utf-8", errors="replace"))
    test_blob = "\n".join(tests)
    if not test_blob:
        print("no test_*.cpp found - is the path right?")
        return 2

    rows = []
    for d in DIRS:
        for path in sorted(d.glob("*.cpp")):
            if path.name.startswith("test_"):
                continue
            src = path.read_text(encoding="utf-8", errors="replace")
            for m in FUNC.finditer(src):
                cls, name = m.group(1), m.group(2)
                if name in ("if", "for", "while", "switch", "catch", "return",
                            "else", "do", "namespace", "sizeof"):
                    continue
                # The body: from the brace to the next function, roughly.
                start = m.end()
                nxt = FUNC.search(src, start)
                body = src[start:nxt.start() if nxt else len(src)]

                touched = set()
                for sub, needles in SUBSYSTEMS.items():
                    if any(n in body for n in needles):
                        touched.add(sub)
                if len(touched) < 2:
                    continue

                # Does any test name it? Method names are searched bare, because
                # a test calls s.join_address(...) not Lsw2ForkSession::join_address.
                if re.search(r"\b%s\b" % re.escape(name), test_blob):
                    continue

                rows.append((len(touched), path.name,
                             (cls + "::" if cls else "") + name,
                             ",".join(sorted(touched))))

    rows.sort(key=lambda r: (-r[0], r[1]))
    print("SEAMS - untested functions that bridge subsystems, widest first\n")
    print("%-3s %-26s %-38s %s" % ("N", "FILE", "FUNCTION", "TOUCHES"))
    print("-" * 108)
    for n, f, fn, subs in rows[:args.top]:
        print("%-3d %-26s %-38s %s" % (n, f[:26], fn[:38], subs))

    print("\n%d seams; showing %d." % (len(rows), min(len(rows), args.top)))
    print("A seam is a place to READ, not a defect. The station -1 bug is what one looked")
    print("like when it was: join_address bridged transport and lockstep, no test")
    print("called it, and it captured the station before the handshake delivered it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
