"""A guard that cannot answer must not be asked.

`ui_fork_build()` returns a file-static that `ui_set_fork_build()` fills in from
the environment - and the shell calls that during ITS main, after the runner has
already called `lsw2_register_dispatch_overrides`. So at registration time the
predicate is FALSE in BOTH executables, always, and any guard written on it
there is inert.

THIS HAS NOW HAPPENED TWICE, in the same function.

  * The fork's menu-string overrides were guarded on `ui_fork_build()` and had
    consequently never been installed even when the table was switched on.
    Several features read as unimplemented when they were merely never
    registered - which is worse than broken, because there is nothing to debug.
    dispatch_overrides.cpp carries a long comment about this at the top of the
    registration function.

  * `lsw2_coopjoin_install()` then made the identical mistake FORTY LINES BELOW
    THAT COMMENT. Its `if (ui_fork_build()) return;` was meant to keep the
    baseline's co-op override out of the fork; being inert, it did not - and
    because the call also sat after `if (!core) return;`, the override
    installed ONLY in the fork and never in the baseline, whose co-op join
    through the master server needs it. The reverse of its contract on
    both sides, from one predicate that reads false too early.

    python tools/sweep_startup_predicate.py

WHAT IT CHECKS. The body of `lsw2_register_dispatch_overrides`, and the bodies
of every `lsw2_*` function it calls, for a live call to `ui_fork_build()`.

WHY IT FOLLOWS THE CALLEES. Checking only the registration function would not
have caught the coopjoin case: the inert guard was inside coopjoin.cpp, one
call away. The first version of this check reported that function as a
violation when it was already fixed, because the FIXED code carries a long
comment explaining the hazard and the comment says `ui_fork_build()` several
times. Comments are stripped before the check for that reason - a scan that
cannot tell code from prose reports the documentation of a bug as the bug.

WHAT TO DO WHEN IT FIRES. Pass the flag in. The caller derives `fork` from the
environment precisely because it must; every callee that needs to know should
take it as a parameter rather than asking a getter that is not ready yet.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRONTEND = ROOT / "port" / "frontend"
ENTRY = "lsw2_register_dispatch_overrides"
PREDICATE = "ui_fork_build"


def strip_comments(text: str) -> str:
    """Block and line comments out, so prose about the hazard is not the hazard.

    Replaced with spaces rather than removed so that any offset into the
    stripped text still lines up with the original.
    """
    def blank(m: re.Match) -> str:
        return re.sub(r"\S", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


def body_of(text: str, at: int) -> str | None:
    """The braced body whose opening brace follows `at`, by brace matching."""
    try:
        i = text.index("{", at)
    except ValueError:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j]
    return None


def find_definition(name: str) -> tuple[str, str] | None:
    """(file name, body) of `name`'s definition, or None. Comments stripped."""
    # The `"` is in the class because the entry point is declared
    # `extern "C" void lsw2_register_dispatch_overrides(...)`, and a prefix
    # pattern without it matched nothing - the check reported the function as
    # renamed rather than reporting on it.
    pattern = re.compile(r"^[A-Za-z_][\w:<>,\s\*&\"]*\b" + re.escape(name) +
                         r"\s*\([^;]*\)\s*\{", re.M)
    for path in sorted(FRONTEND.glob("*.cpp")):
        if path.name.startswith("test_"):
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        m = pattern.search(text)
        if m:
            body = body_of(text, m.start())
            if body is not None:
                return path.name, body
    return None


def main() -> int:
    entry = find_definition(ENTRY)
    if entry is None:
        print(f"{ENTRY} not found - has it been renamed?")
        return 1
    entry_file, entry_body = entry

    checked = 1
    bad = []
    if PREDICATE + "()" in entry_body:
        bad.append(f"{entry_file}: {ENTRY} itself calls {PREDICATE}()")

    for name in sorted({m.group(1) for m in
                        re.finditer(r"\b(lsw2_\w+)\s*\(", entry_body)}):
        found = find_definition(name)
        if found is None:
            continue          # not defined in the frontend - nothing to read
        where, body = found
        checked += 1
        if PREDICATE + "()" in body:
            bad.append(f"{where}: {name}() calls {PREDICATE}(), which is FALSE "
                       f"at registration time in both builds")

    if bad:
        print(f"{PREDICATE}() asked before it can answer:")
        for line in bad:
            print(f"  {line}")
        print(f"\n  {ENTRY} runs from the runner's main, before the shell "
              f"calls\n  ui_set_fork_build(). Pass the flag in as a parameter "
              f"instead - the\n  caller already derives `fork` from the "
              f"environment for this reason.")
        return 1

    print(f"{checked} functions reachable from {ENTRY}, none asks "
          f"{PREDICATE}() before it can answer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
