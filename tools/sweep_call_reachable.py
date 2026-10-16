"""A called function whose CALLER is dead is still dead.

WHY THIS EXISTS, AND WHY IT IS NOT ANY OF THE OTHER FOUR. This repo already
asks four questions about a required feature symbol (sweep_features's table)
and none of them is this one:

    sweep_features         is it defined, and does anything call it?
    sweep_test_only       is the only caller a test?
    sweep_orphans         does it have any use at all?
    sweep_exe_reachable   did the linker keep it?

Every one of those is satisfied by a function called from exactly one place -
a second function that nothing calls. A TWO-LEVEL DEAD CHAIN passes all four,
and this project has shipped one-level versions of that failure repeatedly:
`on_menu_event` stored events nobody read, `lsw2_font_load` was never called,
`Lsw2LanBrowser::expire` was reached only by its test. Each was caught by a
gate built after the fact for that exact shape.

    python tools/sweep_call_reachable.py

WHAT IT DOES. Builds the call graph of port/frontend and port/net from the
source, roots it at the places the PROGRAM actually enters the port, and walks
outward. Any required feature symbol not reached is reported.

THE ROOTS ARE THE WHOLE ARGUMENT, so they are listed explicitly rather than
inferred:

  * every function defined in frontend.cpp - it is the shell, and the runner
    calls into it; if something there is dead, that is a different problem and
    not one this gate can see;
  * every handler named in a dispatch table (kOverrides*, kCoopOverrides) -
    those are called by the recompiler's dispatcher, not by any C caller, so
    they are invisible to a text call-graph and must be seeded;
  * `extern "C"` entry points, which the runner calls by name;
  * ui.cpp's panel and settings entry points, called from frontend.cpp through
    imgui rather than directly.

WHAT IT CANNOT DO. Function pointers other than the dispatch tables, and
virtual dispatch. It also cannot tell a call that runs from one that is
`if (false)`. It answers "is there a path in the source", which is strictly
more than the other four gates ask and strictly less than running the game.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

# A function definition: a line ending in `) {` whose name is an identifier,
# INCLUDING `Class::method`.
#
# THE FIRST VERSION MISSED EVERY METHOD, and reported ten feature functions as
# unreachable because of it. `void Lsw2ForkSession::rebuild_menu() {` has `::`
# inside the name, so a pattern that required a bare identifier after the
# return type matched nothing - and with the whole Lsw2ForkSession layer
# missing from the graph, everything reached only through it looked dead:
# the arena menu, the host menu, the Extras submenu, the console options, the
# kick list, the master browser, the room code screen and the popups.
#
# All of which I had personally traced as reachable earlier the same day. Ten
# implausible results are a broken check, not a discovery.
DEF = re.compile(r"\n(?:[\w:*&<>\[\]]+[ \t]+)+?"
                 r"(?:([A-Za-z_]\w*)::)?([A-Za-z_]\w*)\s*\([^;{}]*\)"
                 r"(?:\s*const)?\s*\{")
CALL = re.compile(r"\b([A-Za-z_]\w{2,})\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "else", "do", "return", "catch",
            "sizeof", "static_cast", "reinterpret_cast", "const_cast",
            "dynamic_cast", "case", "new", "delete", "throw", "and", "or",
            "not", "defined", "assert", "static_assert", "alignof", "typeid"}


def strip_comments(text: str) -> str:
    def blank(m: re.Match) -> str:
        return re.sub(r"\S", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", blank, text)
    # String literals too: "lsw2_foo(" in a log line is not a call.
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', blank, text)


def body_end(text: str, brace: int) -> int:
    depth = 0
    for j in range(brace, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return len(text)


def build_graph():
    """name -> set(names it calls), and name -> file."""
    calls: dict[str, set[str]] = {}
    where: dict[str, str] = {}
    for d in DIRS:
        if not d.is_dir():
            continue
        for path in sorted(d.glob("*.cpp")):
            if path.name.startswith("test_"):
                continue
            text = strip_comments(path.read_text(encoding="utf-8",
                                                 errors="replace"))
            for m in DEF.finditer(text):
                name = m.group(2)
                if name in KEYWORDS:
                    continue
                brace = text.index("{", m.end() - 1)
                body = text[brace:body_end(text, brace)]
                inner = {c for c in CALL.findall(body)
                         if c not in KEYWORDS and c != name}
                calls.setdefault(name, set()).update(inner)
                where.setdefault(name, path.name)
    return calls, where


def roots(calls: dict[str, set[str]]) -> set[str]:
    out: set[str] = set()

    # 1. The shell itself.
    fe = ROOT / "port" / "frontend" / "frontend.cpp"
    if fe.is_file():
        text = strip_comments(fe.read_text(encoding="utf-8", errors="replace"))
        for m in DEF.finditer(text):
            if m.group(2) not in KEYWORDS:
                out.add(m.group(2))

    # 2. Dispatch-table handlers - called by the recompiler, never by C.
    for path in (ROOT / "port" / "frontend" / "dispatch_overrides.cpp",
                 ROOT / "port" / "frontend" / "coopjoin.cpp"):
        if not path.is_file():
            continue
        text = strip_comments(path.read_text(encoding="utf-8",
                                             errors="replace"))
        for tbl in re.finditer(r"NdsDispatchEntry\s+\w+\[\]\s*=\s*\{(.*?)\n\};",
                               text, re.S):
            for row in re.finditer(r"\{\s*0x[0-9A-Fa-f]+u?\s*,\s*\d+\s*,\s*"
                                   r"([A-Za-z_]\w*)", tbl.group(1)):
                out.add(row.group(1))

    # 3. extern "C" entry points the runner calls by name.
    for d in DIRS:
        for path in sorted(d.glob("*.cpp")):
            if path.name.startswith("test_"):
                continue
            text = strip_comments(path.read_text(encoding="utf-8",
                                                 errors="replace"))
            for m in re.finditer(r'extern\s+"C"\s+[\w:*&<>]+\s+([A-Za-z_]\w*)'
                                 r"\s*\([^;]*\)\s*\{", text):
                out.add(m.group(1))

    # 4. ui.cpp's entry points, reached from frontend.cpp through imgui.
    for name in calls:
        if name.startswith("ui_") or name.startswith("lsw2_ui_"):
            out.add(name)
    return out & set(calls) | (out - set(calls))


def main() -> int:
    calls, where = build_graph()
    seen = set()
    queue = list(roots(calls))
    while queue:
        n = queue.pop()
        if n in seen:
            continue
        seen.add(n)
        queue.extend(calls.get(n, ()))

    sys.path.insert(0, str(ROOT / "tools"))
    import sweep_features  # noqa: E402

    bad = []
    for point, what, sym in sweep_features.CLAUSES:
        if sym not in calls:
            continue          # a type or a header-only symbol; not our question
        if sym not in seen:
            bad.append(f"{point}: {what}\n      {sym} is defined in "
                       f"{where.get(sym, '?')} and is NOT REACHABLE from the "
                       f"shell, a dispatch table or an extern \"C\" entry")

    if bad:
        print("feature code with no path from any entry point:")
        for line in bad:
            print(f"  {line}")
        print("\n  Being CALLED is not being REACHED. A function whose only"
              "\n  caller is itself dead passes sweep_features, sweep_test_only,"
              "\n  sweep_orphans and sweep_exe_reachable alike.")
        return 1

    checked = sum(1 for _, _, s in sweep_features.CLAUSES if s in calls)
    print(f"{len(seen)} functions reachable from the entry points; all "
          f"{checked} feature functions among them")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
