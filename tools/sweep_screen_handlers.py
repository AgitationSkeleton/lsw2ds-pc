"""A screen that can be opened must be handled, and vice versa.

WHY THIS EXISTS. The most expensive bug this port has shipped was
`Lsw2ForkSession::on_menu_event` never being called: every fork menu moved its
cursor and did nothing, and all of the fork's in-game menus were affected. That one is
fixed and `sweep_test_only.py` now catches its exact shape - a handler nothing
reaches.

This catches the next size down, which the same day's audit showed is one edit
away. `Lsw2ForkScreen` has eight real values, and each needs TWO things that
live hundreds of lines apart:

    screen_ = Lsw2ForkScreen::X      somewhere that opens it
    screen_ == Lsw2ForkScreen::X     a branch in on_menu_event

A screen with the first and not the second opens, draws its rows, and drops
every button press - indistinguishable from the on_menu_event bug from the
player's side. A screen with the second and not the first is a handler that can
never run, which is dead weight rather than a defect, but is worth knowing about
because it usually means an entry point was removed and its branch left behind.

    python tools/sweep_screen_handlers.py

WHY A GATE RATHER THAN CARE. Adding a screen is exactly when this is easy to get
wrong: the enum, the opener and the handler are three separate edits in two
files, and the compiler is happy with any two of them. Nothing else in the suite
would notice - the handler function is called, the enum value is used, and no
value is stored unread.

A SCREEN WITH NO MENU is handled by its own path rather than by on_menu_event,
and the comparison it is found by lives there instead: the Character screen
opens no menu - the cartridge's Free Play grid is the host's
picker - and finish_character_pick is where it is handled and closed.

WHAT IT CANNOT DO. Tell whether the handler is CORRECT, or whether every ROW of
a screen is acted on. Row coverage needs to know which rows are value rows
synced in bulk and which are actions matched by id, and getting that wrong would
report working menus as broken - the failure this suite's own history warns
about most. The screen level is mechanical and worth having on its own.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "port" / "frontend" / "forksession.h"
SOURCE = ROOT / "port" / "frontend" / "forksession.cpp"

ENUM = "Lsw2ForkScreen"

# `None` is the closed state: it is assigned everywhere and handled by an early
# return rather than a branch, so it plays by different rules.
NOT_A_SCREEN = {"None"}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def screens(header: str) -> list[str]:
    m = re.search(r"enum class " + ENUM + r"\s*\{(.*?)\}", header, re.S)
    if not m:
        return []
    out = []
    for part in m.group(1).split(","):
        name = re.match(r"\s*([A-Za-z_]\w*)", part)
        if name:
            out.append(name.group(1))
    return out


def main() -> int:
    if not HEADER.is_file() or not SOURCE.is_file():
        print(f"missing {HEADER.name} or {SOURCE.name}")
        return 1

    header = strip_comments(HEADER.read_text(encoding="utf-8",
                                             errors="replace"))
    source = strip_comments(SOURCE.read_text(encoding="utf-8",
                                             errors="replace"))

    names = [n for n in screens(header) if n not in NOT_A_SCREEN]
    if not names:
        print(f"no {ENUM} values found in {HEADER.name} - renamed?")
        return 1

    bad = []
    for name in names:
        qualified = ENUM + "::" + name
        # An opener assigns it; a handler compares against it. The comparison
        # may be written either way round, and `parent_` restores are
        # assignments too - which is why an opener is "any assignment".
        # THE LOOKBEHIND IS NOT DECORATION. `=\s*Name` also matches the second
        # `=` of `== Name`, so without it every comparison counted as an
        # assignment and the "handler with no opener" branch could never fire.
        # Found by negative-testing that branch and watching it stay silent -
        # a check that cannot report is worse than no check.
        opened = re.search(r"(?<![=!<>])=\s*" + re.escape(qualified),
                           source) is not None
        handled = re.search(r"==\s*" + re.escape(qualified),
                            source) is not None
        if not opened and not handled:
            bad.append(f"{name} is declared and neither opened nor handled")
        elif not opened:
            bad.append(f"{name} has a handler branch but nothing opens it - "
                       f"an entry point was probably removed")
        elif not handled:
            bad.append(f"{name} can be opened but on_menu_event has no branch "
                       f"for it: its rows would draw and every press would be "
                       f"dropped")

    if bad:
        print("fork screens that would not work:")
        for line in bad:
            print(f"  {line}")
        print("\n  This is the on_menu_event bug one size down - a menu that"
              "\n  appears, draws its rows, and does nothing at all.")
        return 1

    print(f"all {len(names)} fork screens are both opened and handled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
