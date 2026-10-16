"""Every `lsw2_*` function DECLARED in a port header, and who calls it.

WHY THIS EXISTS. "Declared and never called" is the most-repeated defect in this
project - six instances now, three of them written by the same hand that wrote
the sweep for the previous one. A function that is built, documented, compiled
and linked but never reached costs exactly as much as a working one and does
nothing, and the failure is invisible: the build is green and the tests pass,
because a test IS a caller.

So the check has to be "who calls it in the SHIPPING build", which means
excluding test_*.cpp - a function whose only caller is its own test is an orphan
wearing a disguise.

    python tools/sweep_orphans.py [--all]

Without --all it prints only the orphans. Exit status is 1 if any are found, so
it can be a gate rather than a report.

WHAT IT DOES NOT CLAIM. This is a text sweep, not a linker. A function reached
only through a function pointer, a dispatch table entry or a vtable will look
like an orphan, and several legitimately are - the dispatch overrides are named
in a table and never called by name. Those are listed as KNOWN below rather than
filtered by a pattern, because a pattern would silently absorb a real orphan the
day someone named a new function similarly.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

# Reached through a table, a callback or the framework rather than by name.
# Listed one by one on purpose - see the module docstring.
KNOWN = {
    # (lsw2_fork_note_script_target was here: the enqueue observer's note of
    # the popups' script target. The popups stopped using a script queue -
    # see dispatch_overrides.cpp - and the note went with them.)

    # EXERCISED ONLY BY TESTS, and that is now the correct state.
    #
    # It was the port's faithful reimplementation of the guest's enqueue, called
    # from the override on 0x02060084. That override could never run (the
    # recompiler calls same-bank targets by symbol), and its replacement on
    # 0x02060158 DELEGATES to the lifted function instead of reimplementing it -
    # which is strictly better, and leaves this with no production caller.
    #
    # Kept rather than deleted because eight cases in test_scriptqueue.cpp pin
    # the queue's packing through it. It USED to say that behaviour was what the
    # fork's own popup route depended on, via lsw2_script_enqueue_values; the
    # popups now go to the game's message display instead (no popup queued into
    # a script object was ever seen on screen), and _values went with them. So
    # the pair below is documentation of the guest's format, pinned by tests.
    "lsw2_script_enqueue",
    "lsw2_script_enqueue_with",   # the template behind it, same status
}

# THE BASELINE, and why it is a list of names rather than a threshold.
#
# When this sweep was first run correctly it found twenty-one orphans. Twenty
# were still there after the twenty-first was fixed, and a gate that fails on
# the day it is added is a gate that gets switched off - so they are recorded
# here and the gate guards against the twenty-second.
#
# EVERY ONE WAS CHECKED INDIVIDUALLY, against the one question that matters:
# is this an unwired FEATURE or a dead accessor? All twenty are accessors, and
# no required feature is unwired. Three worth naming because they
# looked alarming and were not:
#
#   lsw2_cheats_write_flags   the unlock-everything path does not go through it -
#                             lsw2_cheats_unlock_everything() does, and IS called
#   (lsw2_extra_name was here, and its note - "duplicates a table the Extras
#   builder reads direct" - described a real structural problem rather than a
#   dead accessor. The builder has been pointed at the accessor, which is now
#   where the cartridge-decoded name arrives, so it is off this list.)
#   lsw2_select_character_at  the forward direction of a page/slot mapping the
#                             shipping code only ever needs backwards
#   (lsw2_font_load was here too, and being on this list is what let it stay
#   broken. It is not an accessor nobody needs: port/build.sh installs font_18
#   beside the executable and every text path in gamemenu_render.cpp tests
#   lsw2_font_ready() first, so with nothing calling the loader the fork's
#   menus drew their frames and no labels. Waiving it as "an orphan" answered
#   the wrong question - the question was not whether anything called it but
#   whether anything SHOULD. tools/sweep_test_only.py asks that one, because
#   the tests were calling it the whole time.)
#
# A NAME LIST, NOT A COUNT, on purpose: a count would let a new orphan in as
# soon as an old one was deleted, which is the silent-absorption failure the
# module docstring warns about for patterns. Deleting dead code shortens this
# list; adding to it is a deliberate act that shows up in a diff.
BASELINE = {
    "lsw2_cheats_set_level",          # kept as the documented contrast to the warp
    "lsw2_cheats_write_flags",
    "lsw2_font_atlas_height",
    "lsw2_fork_boot_status",          # the boot overlay, since removed
    "lsw2_gl_stop",
    "lsw2_net_local_station",
    "lsw2_ping_one",
    "lsw2_room_code_alphabet",
    "lsw2_room_code_alphabet_size",
    "lsw2_select_character_at",
    "lsw2_widescreen_enabled",
    "lsw2_widescreen_frustum_patched",
}

DECL = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?\b(lsw2_[a-z0-9_]+)\s*\(", re.M)


def headers():
    for d in DIRS:
        if d.is_dir():
            yield from sorted(d.glob("*.h"))


def sources():
    for d in DIRS:
        if d.is_dir():
            for p in sorted(list(d.glob("*.cpp")) + list(d.glob("*.h"))):
                if p.name.startswith("test_"):
                    continue
                yield p


def main() -> int:
    show_all = "--all" in sys.argv

    declared: dict[str, Path] = {}
    for h in headers():
        text = h.read_text(encoding="utf-8", errors="replace")
        # Strip block comments so a name mentioned in prose is not a declaration.
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for name in DECL.findall(text):
            declared.setdefault(name, h)

    # Count uses across the shipping build, with comments stripped for the same
    # reason: this project's headers discuss functions by name at length, and a
    # mention is not a call.
    bodies: dict[Path, str] = {}
    for p in sources():
        t = p.read_text(encoding="utf-8", errors="replace")
        bodies[p] = re.sub(r"/\*.*?\*/", "", t, flags=re.S)

    orphans = []
    rejoined = []          # baselined names that have since gained a caller
    for name, home in sorted(declared.items()):
        # COUNT EVERY MENTION, THEN SUBTRACT THE TWO THAT ARE NOT USES: the
        # declaration in the header and the definition in some .cpp.
        #
        # The first version excluded the whole home module - the header's stem
        # and its `_policy` sibling - and that was wrong in a way that made the
        # sweep useless. A pure policy function IS legitimately reached by its
        # own module's guest half, so excluding the sibling hid the caller
        # rather than the orphan, and forty live functions were reported dead.
        word = re.compile(r"\b" + re.escape(name) + r"\b")
        total = 0
        where = []
        for p, t in bodies.items():
            n = len(word.findall(t))
            if n:
                total += n
                where.append(p.name)
        uses = total - 2                    # the declaration and the definition
        if show_all:
            print(f"{name:<44} {uses:>2}  {', '.join(where[:4])}")
        if uses <= 0 and name not in KNOWN and name not in BASELINE:
            orphans.append((name, home.name))
        if uses > 0 and name in BASELINE:
            rejoined.append(name)

    if orphans:
        print(f"\n{len(orphans)} DECLARED AND NEVER CALLED:")
        for name, home in orphans:
            print(f"  {name}   (declared in {home})")
        return 1

    # THE BASELINE HAS TO ROT VISIBLY OR IT BECOMES A DUMPING GROUND. Two ways
    # an entry goes stale: the function gained a caller, or it was deleted. Both
    # mean the name should come off the list, and both are failures here - not
    # because anything is broken, but because a list nobody prunes is a list
    # nobody reads, and the next real orphan hides in it.
    stale = sorted(set(rejoined))
    vanished = sorted(BASELINE - set(declared))
    if stale:
        print("\nbaselined but now CALLED - take them off the list:")
        for name in stale:
            print(f"  {name}")
    if vanished:
        print("\nbaselined but no longer declared - take them off the list:")
        for name in vanished:
            print(f"  {name}")
    if stale or vanished:
        return 1

    live = len(BASELINE & set(declared))
    print(f"\nno new orphans - {len(declared)} declarations, {live} baselined")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
