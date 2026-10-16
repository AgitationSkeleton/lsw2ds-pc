"""A function the tests reach and the game does not is a feature nobody has.

WHY THIS EXISTS. Three bugs of one shape have now shipped, and every one of
them was written correctly, tested, and never reached:

  * `Lsw2ForkSession::on_menu_event` - `lsw2_menuhost_feed_keys` stored each
    menu event in a file-static and NOTHING READ IT, so every Activated and
    ValueChanged was dropped. All of the host settings' in-game UI moved its cursor and did
    nothing. The tests called the handler directly and passed.
  * `lsw2_font_load` - `port/build.sh` exports font_18 and installs it beside
    the executable, saying in its own comment that the port needs it there "or
    its menus fall back to nothing". Nothing opened it, so `lsw2_font_ready()`
    was false all run and every text path returned early: the fork's menus drew
    their frames with no labels on them. `test_gamefont.cpp` loaded it happily.
  * `Lsw2LanBrowser::expire` - dead LAN hosts were never dropped, so the
    browser listed lobbies that were not there. `test_lan.cpp` called it.

The common shape is not "dead code". Every one of these had a caller; the
caller was a test. That is invisible to `sweep_orphans`, which asks whether a
declaration has any use at all, and invisible to a green test suite, which is
exactly the thing doing the calling.

    python tools/sweep_test_only.py

WHAT IT CHECKS. Every function defined in `port/frontend/*.cpp` or
`port/net/*.cpp` that is called from a `test_*.cpp` and from nowhere else.

WHY THE CALL DETECTOR IS THE WHOLE DIFFICULTY. Telling a call from a
declaration by looking at the text before the name does not work, and the first
three versions of this sweep each reported a different wrong answer:

  * `x = f(...);` has `=` before the name, so a head-fragment test read it as a
    prototype and every assigned-from call vanished - `lsw2_pause_escape` and
    39 others were reported as test-only when production calls them.
  * `    f();` has only whitespace before the name, and a declaration pattern
    whose type portion can match whitespace swallows it - which hid
    `open_host_settings`, `peer_joined` and `lsw2_ping_sweep`, all called from
    forksession.cpp.

So the test is on the WHOLE LINE, and the type portion is required to contain a
word character. 40 reported entries became 13, and the 13 were hand-checked one
at a time - which is how the font and the browser were found among eleven
legitimate ones.

WHAT IT CANNOT DO. Say whether an entry is a BUG. Most are not: this project
deliberately splits pure policy into `*_policy.cpp` so it can be tested without
a ROM, and a table lookup verified against the cartridge is worth keeping even
if today's caller does not need it. That judgement is a person's, which is what
ALLOWED is - each entry carries the reason it was cleared, so a future reader
inherits the check instead of repeating it.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

DEF = re.compile(r"\n(?:static\s+)?[\w:*&<>\s]+?(\w+)\s*\([^;]*?\)\s*\{")

# AN INLINE MEMBER DEFINED IN A HEADER, which DEF above cannot see because it
# only scans .cpp files. That blind spot is how Lsw2LockstepSession::state()
# escaped this sweep entirely: it is never called in production, and nothing
# here noticed.
INLINE = re.compile(
    r"^\s*(?:static\s+|virtual\s+|constexpr\s+|inline\s+)*"
    r"[\w:<>,*&\s]+?[\s*&](\w+)\s*\(([^;{)]*)\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?\{", re.M)

# `if (...) {` matches the definition shape exactly. Same list, and the same
# reason, as sweep_log_reachable.py.
KEYWORDS = {"if", "for", "while", "switch", "else", "do", "return", "catch",
            "sizeof"}

# A NAME THIS SWEEP CANNOT DECIDE, because two classes both declare it inline
# and every check here works on the name alone. `session.state()` and
# `lockstep_->state()` are one string to a regex, so a call to either clears
# both - which is not noise, it is BLINDNESS, and it is how
# Lsw2LockstepSession::state() went unexamined.
#
# An entry is a claim that a person read every declaration of that name and
# every call site. A new ambiguous name with no entry FAILS the sweep, so the
# blind spot cannot grow silently.
AMBIGUOUS = {
    "state": "Lsw2ForkSession::state() is consumed all over ui.cpp and the "
             "per-frame modules. Lsw2LockstepSession::state() is called by "
             "NOTHING in production - state_ is write-only there - and that is "
             "accepted: the fork session detects a drop itself at "
             "forksession.cpp:522 and calls both lockstep_->drop() and "
             "peer_left_at(), so no signal is lost. See also "
             "sweep_enum_handled.py, which clears Lsw2NetState::Connecting for "
             "the neighbouring reason.",
    "kind": "both used - session.kind() at ui.cpp:860, and forksession.h's own "
            "kind() forwards to master_.kind()",
    "last_error": "both used - session.last_error() in ui.cpp, "
                  "master_.last_error() at forksession.cpp:407",
    "servers": "both used - master_.servers() and browser_.servers(), at "
               "forksession.cpp:125 and :162",
    "running": "TWO CLASSES share the name and no name-only check can tell "
               "their calls apart. Lsw2LanBrowser::running() (lsw2_lan.h) is "
               "used in production (the LAN browse path). "
               "Lsw2WirelessRelay::running() (lsw2_wireless_relay.h) is called "
               "only by test_wireless_relay.cpp today, and that is ACCEPTED: "
               "the relay is now WIRED (lsw2_relay_session drives it from the "
               "frontend loop under LSW2_RELAY, and forwarded()/injected() have "
               "production callers via the session's status line), but the "
               "session tracks its own phase and never calls the relay's own "
               "running(), so that one accessor stays test-only. The UI trigger "
               "(directory/room-code/direct-IP) is the remaining piece.",
}

# Hand-checked and cleared, with the reason. Not an exemption list: an entry
# here is a claim that somebody read the code and decided, and it is meant to
# be re-read when the surrounding design changes.
ALLOWED = {
    # Pure lookups over tables read out of the cartridge. Unused today because
    # the last hunter is applied by writing the game's own pending-character
    # field by id (lasthunter_observe.cpp:149), which needs no page or slot.
    # Kept because they encode a later correction to an earlier wrong
    # inference about the select screen's ordering - a verified fact that would
    # have to be re-derived if deleted.
    "lsw2_select_character_at": "the same table lsw2_hunter_page_slot reads, "
                                "read the other way; the last hunter is "
                                "applied by character id instead",

    # Test-facing views of constants the module uses internally by name.
    "lsw2_room_code_alphabet": "kAlphabet is used directly inside joinmenu.cpp",
    "lsw2_room_code_alphabet_size": "as above",

    # Convenience wrappers over the primitive production actually wants.
    "lsw2_ping_one": "one target; refresh_lobbies sweeps them all at once",

    # EXPORTED GETTERS WHOSE STATE THE CLASS USES DIRECTLY. Both members are
    # busy - screen_ drives the menu switch in forksession.cpp and delay_ sets
    # the lockstep's consuming frame - but the code that needs them is inside
    # the class, which reaches them by name. Only the public accessor is
    # unused, and an accessor is a smaller thing to leave standing than a
    # member: nothing can be out of step with it.
    #
    # Contrast frame() and stalls(), which were on this list until the session
    # teardown started reporting them. Those two were a MEASUREMENT nobody
    # could see; these two are a door nobody has needed to open.
    "delay_frames": "delay_ is read inside step(); no setting exists to change "
                    "it, which is why set_delay_frames is on this list too",

    # The menu's geometry comes from the game's own row layout via
    # Lsw2Menu::row_y, not from the font - the font supplies glyphs and widths
    # only. These two are the metrics that layout would need and does not.
    "lsw2_font_atlas_height": "the renderer walks glyph rects, not the atlas",

    # delay_ starts at kLsw2MinDelayFrames and nothing raises it. That is the
    # lowest-latency default and there is no setting for it in either build, so
    # this is an unused setter rather than an unwired control.
    "set_delay_frames": "lockstep runs at the minimum delay; no setting exists",

}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def calls(fn: str, blob: str) -> int:
    """Occurrences of `fn(` that are neither its definition nor a declaration.

    THE TEST IS ON THE WHOLE LINE. A declaration is a line that is nothing but
    a declaration; anything else containing `fn(` is a use. Testing the text
    immediately before the name instead - which is what the first versions did
    - cannot tell `void f();` from `x = f();` or from a bare `f();`, and both
    mistakes hide real calls rather than inventing fake ones, so the sweep
    reports healthy code as broken and gets switched off.
    """
    proto = re.compile(
        r"^\s*(?:static|extern|inline|virtual)?\s*[\w:<>,&*]+[\w:<>,&*\s]*\s+"
        + re.escape(fn) + r"\s*\([^;{]*\)\s*(?:const\s*)?;\s*$")
    found = 0
    for m in re.finditer(r"(^|[^\w:])" + re.escape(fn) + r"\s*\(", blob, re.M):
        start = blob.rfind("\n", 0, m.start()) + 1
        end = blob.find("\n", m.start())
        line = blob[start:end if end >= 0 else len(blob)]
        tail = blob[m.end():m.end() + 300]
        after = tail[len(tail.split(")")[0]) + 1:].lstrip()
        # A CONST MEMBER'S DEFINITION HAS WORDS BETWEEN THE PARENS AND THE
        # BRACE. `int f() const { ... }` did not match `after.startswith("{")`,
        # so the definition counted as a call to itself - and since most inline
        # accessors are const, widening this sweep to header inlines found
        # nothing at all until this was fixed. Caught by negative-testing the
        # widening and watching the branch fail to fire.
        after = re.sub(r"^(?:const|noexcept|override|final|&|&&)\s*", "",
                       after)
        after = re.sub(r"^(?:const|noexcept|override|final|&|&&)\s*", "",
                       after)
        if after.startswith("{"):
            continue                        # the definition itself
        if proto.match(line):
            continue                        # a declaration
        found += 1
    return found


def main() -> int:
    production: dict[str, str] = {}
    tests: dict[str, str] = {}
    for directory in DIRS:
        if not directory.is_dir():
            continue
        for path in sorted(list(directory.glob("*.cpp")) +
                           list(directory.glob("*.h"))):
            text = strip_comments(path.read_text(encoding="utf-8",
                                                 errors="replace"))
            (tests if path.name.startswith("test_") else production)[
                path.name] = text

    if not production:
        print("no sources found under port/frontend or port/net")
        return 1

    defined: dict[str, str] = {}
    for name, text in production.items():
        if not name.endswith(".cpp"):
            continue
        for m in DEF.finditer(text):
            if m.group(1) not in KEYWORDS:
                defined.setdefault(m.group(1), name)

    # INLINE MEMBERS TOO, and a note of which headers each name is declared in
    # - because a name declared in TWO classes cannot be checked by name at
    # all, and saying so is the whole point of gathering it.
    inline_headers: dict[str, set[str]] = {}
    for name, text in production.items():
        if not name.endswith(".h"):
            continue
        for m in INLINE.finditer(text):
            fn = m.group(1)
            if fn in KEYWORDS:
                continue
            inline_headers.setdefault(fn, set()).add(name)
            defined.setdefault(fn, name)

    prod_blob = "\n".join(production.values())
    test_blob = "\n".join(tests.values())

    bad, cleared = [], set()
    for fn in sorted(defined):
        if calls(fn, prod_blob) or not calls(fn, test_blob):
            continue
        if fn in ALLOWED:
            cleared.add(fn)
            continue
        bad.append(f"{fn} ({defined[fn]}) is called only from tests")

    for stale in sorted(set(ALLOWED) - cleared):
        bad.append(f"ALLOWED lists {stale!r}, which production now calls or "
                   f"which no longer exists - take it off the list")

    # A NAME TWO CLASSES SHARE CANNOT BE DECIDED HERE, so it must be decided by
    # a person, once, in writing. Reporting it is the only honest option: the
    # alternative is what this sweep did before, which was to clear both
    # classes' method because one of them was called.
    shared = {fn for fn, hs in inline_headers.items() if len(hs) > 1}
    for fn in sorted(shared - set(AMBIGUOUS)):
        where = ", ".join(sorted(inline_headers[fn]))
        bad.append(f"{fn} is declared inline in {where} - two classes share "
                   f"the name, so no check here can tell their calls apart. "
                   f"Read both and add an AMBIGUOUS entry saying what you "
                   f"found")
    for stale in sorted(set(AMBIGUOUS) - shared):
        bad.append(f"AMBIGUOUS lists {stale!r}, which is no longer declared in "
                   f"more than one header - take it off the list")

    if bad:
        print("reached by the tests and not by the game:")
        for line in bad:
            print(f"  {line}")
        print("\n  Each of these is a LEAD, not a verdict - read it and decide."
              "\n  on_menu_event, lsw2_font_load and Lsw2LanBrowser::expire all"
              "\n  looked like noise on this list and all three were the bug.")
        return 1

    print(f"{len(defined)} production functions checked; none are reached only "
          f"by tests ({len(cleared)} cleared by hand, each with a reason)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
