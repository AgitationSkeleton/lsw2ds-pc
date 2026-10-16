"""A diagnostic that cannot print is worse than no diagnostic.

WHY THIS EXISTS. Twice this project has shipped a measurement that could not
report, and both times the code was correct:

  * `statelog.h` was gated on the fork build, and the two states it existed to
    name are reached through the Cantina console and the pause wireless row -
    which the fork warps past and hides. It watched the one build with both
    subjects removed. Caught after shipping.
  * `statereq.h` was about to go into `kOverridesCore`, which outside
    `LSW2_HLE=1` installs only in the fork, for exactly the same subjects.
    Caught before shipping, by remembering the first one.

The failure is expensive out of proportion to its size. A real test run with
live players is scarce, and a line that cannot print spends a run and returns nothing, while looking from the source like it
worked.

    python tools/sweep_log_reachable.py

WHAT IT CHECKS. Every function in `port/frontend/*.cpp` that prints a tagged
line - `[fork]`, `[diag]`, `[state]`, `[hle]`, `[trace]` - must be reachable in
one of the three ways this port reaches things:

  1. called by name somewhere;
  2. listed in a dispatch override table, which is how the guest reaches it;
  3. passed as a bare identifier to a setter - `lsw2_set_popup_sink(f)` - which
     is how the port installs a sink.

All three are real, and a check that knew only the first would report seven
override handlers and two callbacks as dead. That kind of noise is what gets a
sweep switched off, so the other two are recognised rather than exempted.

WHAT IT CANNOT DO. Tell whether the enclosing function runs in the BUILD or at
the MOMENT the line is needed - `statelog`'s bug was that it ran in the wrong
build, and this would have passed it. `tools/sweep_build_gating.py` is what
covers that half. This one catches a printer nothing reaches at all.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRONTEND = ROOT / "port" / "frontend"

TAG = re.compile(r'"\[(?:fork|diag|state|hle|trace)\][^"]*"')
# The trailing `(?:const|noexcept|override|final)` is not decoration. Without
# it `void C::f() const {` is not a definition to this regex, so a print inside
# one is attributed to whatever function was defined ABOVE it - which is
# usually called, so the print looks reached. Third place the same const blind
# spot turned up in one pass; the other two are in sweep_test_only.py.
DEF = re.compile(r"\n(?:static\s+)?[\w:*&<>\s]+?(\w+)\s*\([^;]*?\)\s*"
                 r"(?:(?:const|noexcept|override|final)\s*)*\{")


# `if (...) {` matches the definition shape exactly, and the first version of
# this file recorded every print sitting after one as belonging to a function
# called `if` - which is "called" everywhere, so the whole sweep passed
# vacuously. Found by asking it which function a known print belonged to
# instead of trusting the count it reported.
KEYWORDS = {"if", "for", "while", "switch", "else", "do", "return", "catch",
            "sizeof"}


def enclosing(text: str, at: int) -> str | None:
    """The name of the function containing `at`, or None."""
    best = None
    for m in DEF.finditer(text, 0, at):
        if m.group(1) not in KEYWORDS:
            best = m
    return best.group(1) if best else None


def main() -> int:
    sources = {}
    for p in sorted(FRONTEND.glob("*.cpp")) + sorted(FRONTEND.glob("*.h")):
        if p.name.startswith("test_"):
            continue
        # COMMENTS OUT FIRST. Without this the definition scan matched prose -
        # "the guest is settled between frames" made a function called `guest`
        # and reported it as an unreachable printer. Same strip sweep_features
        # does, and for the same reason.
        raw = p.read_text(encoding="utf-8", errors="replace")
        raw = re.sub(r"/\*.*?\*/", "", raw, flags=re.S)
        raw = re.sub(r"//[^\n]*", "", raw)
        sources[p.name] = raw
    if not sources:
        print(f"no sources under {FRONTEND}")
        return 1

    blob = "\n".join(sources.values())

    printers = {}
    for name, text in sources.items():
        # HEADERS TOO. This scanned only .cpp files, which is the same blind
        # spot sweep_test_only.py had - a tagged line inside an inline member
        # defined in a header was invisible, and so was the question of whether
        # anything called it. No header prints one today, so this changes
        # nothing now; it is here so that the first one to do so is checked
        # rather than silently exempt.
        if not (name.endswith(".cpp") or name.endswith(".h")):
            continue
        for m in TAG.finditer(text):
            fn = enclosing(text, m.start())
            if fn:
                printers.setdefault(fn, set()).add(name)

    bad = []
    for fn in sorted(printers):
        # A PROTOTYPE IS NOT A CALL. `void lsw2_freezewatch_tick();` in a
        # header matches `fn(` exactly as a call does, so counting mentions
        # made every declared printer look reached - the same bug
        # sweep_features had, found here the same way: by negative-testing and
        # watching the branch not fire. A call is followed by an argument list
        # that does NOT end the statement immediately after the parens.
        called = False
        for m in re.finditer(r"(^|[^\w:])" + re.escape(fn) + r"\s*\(",
                             blob, re.M):
            tail = blob[m.end():m.end() + 200]
            args = tail.split(")")[0]
            after = tail[len(args) + 1:].lstrip()
            # A const member's definition has words before its brace -
            # `void C::f() const {` - so a bare startswith("{") reads it as a
            # call to itself and the printer inside it looks reached. Same fix
            # as sweep_test_only.py, where this bug made a whole widening
            # report nothing at all.
            after = re.sub(r"^(?:const|noexcept|override|final)\s*", "", after)
            after = re.sub(r"^(?:const|noexcept|override|final)\s*", "", after)
            if after.startswith("{"):
                continue                  # the definition
            head = blob[max(0, m.start() - 60):m.start()].rstrip()
            # ":" is in the list because a call can follow a `case X:` label -
            # `lsw2_host_menu_build` is called that way in rebuild_menu, and
            # without it three real calls read as prototypes.
            #
            # "=" IS IN IT BECAUSE `int x = f();` IS A CALL, and without it the
            # sweep read every assignment-form call as a prototype and reported
            # the callee dead. For example, lsw2_wh_make_view_manager is
            # called as `const int vm = lsw2_wh_make_view_manager();` and the
            # sweep failed the build saying "nothing calls, tables or installs
            # it". A declaration never has "=" before the name, so this cannot
            # let a real prototype through - and the hole was not specific to
            # one function, it was every call whose result is assigned.
            if after.startswith(";") and head and not head.endswith(
                    (";", "{", "}", ")", ",", "(", ":", "=")):
                continue                  # a prototype
            called = True
            break
        # A dispatch table row: `{ 0x..., 0, fn, nullptr },`
        tabled = re.search(r"\{\s*0x[0-9A-Fa-f]+u?\s*,\s*\d+\s*,\s*"
                           + re.escape(fn) + r"\s*,", blob) is not None
        # PASSED AS A CALLBACK, in any argument position. The first version
        # only matched a lone argument - `lsw2_set_popup_sink(f)` - and missed
        # `lsw2_netmsg_walk(base, guest_read32, nullptr, report)`, where the
        # function is the fourth. A bare identifier between a comma or paren
        # and a comma or paren is a reference, not a call.
        installed = re.search(r"[(,]\s*" + re.escape(fn) + r"\s*[,)]",
                              blob) is not None

        if not (called or tabled or installed):
            bad.append(f"{fn} prints a tagged line but nothing calls, tables "
                       f"or installs it  ({', '.join(sorted(printers[fn]))})")

    if bad:
        print("diagnostics that cannot print:")
        for line in bad:
            print(f"  {line}")
        print("\n  A live test run is scarce, and a line that cannot print spends a"
              "\n  run and returns nothing while looking like it worked.")
        return 1

    print(f"all {len(printers)} tagged-line printers are reachable "
          f"(by call, override table, or sink)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
