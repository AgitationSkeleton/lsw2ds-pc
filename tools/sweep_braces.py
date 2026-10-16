#!/usr/bin/env python3
"""A braceless if/for/while whose body READS as two statements and compiles as one.

WHY THIS EXISTS. dispatch_overrides.cpp captured the Free Play select widget
for the party portrait with a one-statement `if`:

    if (g_cpu.R[14] == kFreePlayCtorReturn)
        lsw2_partyportrait_note_widget(g_cpu.R[4]);

The host settings' character picker then wanted the same pointer, and the second call went in
under the first at the same indent - outside the condition. The indentation
said "both of these happen when the Free Play constructor returns"; the
compiler said "the second one happens every time". So the widget pointer was
being set from a callee-saved register on every save-flag query, and the
main-RAM range check did not save it: any plausible-looking word would have
been recorded as "the widget" and the host's character read out of it.

The compiler is silent about this by design, every test passed, and all
sixty-one gates were green. Nothing but reading the line catches it, which is
why it is now a gate.

WHAT IT LOOKS FOR, precisely: a controlling statement with no brace, a body of
exactly one statement, and then ANOTHER statement at the body's own indent.
That last part is what separates the bug from ordinary braceless code -

    if (!stations_[i].present) continue;          <- fine, body on the same line
    if (page < kHunterPageLow)
        page = kHunterPageLow;                    <- fine, one statement, then
    ++hits;                                          a line at the OUTER indent

TWO FALSE POSITIVES SHAPED THIS. The first version reported five hits and four
were `if (cond) {   /* note */` - braced, with the brace not last on the line
because a comment followed it. The fifth was lockstep.cpp's

    for (const Pending& p : queue_[i])
        if (p.frame == consuming) { have = true; break; }

where the body ends in `}` and the walk looking for `;` ran straight past it
into the next statement. Five implausible results are a broken check, not five
bugs; both are handled below and both are why this file says so.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

CTRL = re.compile(r"^(\s*)(if|for|while)\s*\(")
TRAILING_BLOCK = re.compile(r"/\*.*?\*/\s*$")
TRAILING_LINE = re.compile(r"//.*$")


def indent(line):
    return len(line) - len(line.lstrip())


def is_filler(line):
    """Blank, or a comment line - neither is the body."""
    return not line.strip() or line.lstrip().startswith(("/*", "*", "//"))


def scan(path):
    """Yield (line_no, condition, next_line_no, next_line) for each hit."""
    text = path.read_bytes().decode("utf-8", "replace").replace("\r\n", "\n")
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        m = CTRL.match(lines[i])
        if not m:
            i += 1
            continue

        # Walk to the end of the condition - it can span lines.
        depth = 0
        j = i
        while j < len(lines):
            depth += lines[j].count("(") - lines[j].count(")")
            if depth <= 0:
                break
            j += 1
        if j >= len(lines):
            break

        # STRIP A TRAILING COMMENT BEFORE ASKING WHETHER IT IS BRACED.
        tail = TRAILING_LINE.sub("", TRAILING_BLOCK.sub("", lines[j])).rstrip()
        if tail.endswith("{") or tail.endswith(";"):
            i = j + 1
            continue

        base = len(m.group(1))
        k = j + 1
        while k < len(lines) and is_filler(lines[k]):
            k += 1
        if k >= len(lines) or indent(lines[k]) <= base:
            i = j + 1
            continue

        body_indent = indent(lines[k])

        # Find where that one statement ends. It ends on `;` or on `}` - the
        # brace case being a one-line block, which a `;`-only walk overruns.
        e = k
        depth = lines[e].count("(") - lines[e].count(")")
        while e < len(lines) - 1 and (
            depth > 0 or not lines[e].rstrip().endswith((";", "}"))
        ):
            e += 1
            depth += lines[e].count("(") - lines[e].count(")")

        n = e + 1
        while n < len(lines) and is_filler(lines[n]):
            n += 1
        if n < len(lines) and lines[n].strip() and indent(lines[n]) == body_indent:
            nxt = lines[n].lstrip()
            if not nxt.startswith(("}", "else", "#")):
                yield (i + 1, lines[i].strip(), n + 1, nxt[:70])

        i = j + 1


def main():
    files = []
    for d in SRC:
        files += sorted(list(d.glob("*.cpp")) + list(d.glob("*.h")))

    bad = []
    for f in files:
        for ln, cond, ln2, nxt in scan(f):
            rel = f.relative_to(ROOT).as_posix()
            bad.append(
                f"{rel}:{ln}  {cond}\n"
                f"      body ends, and then {rel}:{ln2}  {nxt}\n"
                f"      that second line is OUTSIDE the condition - brace it "
                f"or outdent it"
            )

    if bad:
        print("braceless bodies that read as two statements and compile as one:")
        for line in bad:
            print(f"  {line}")
        print(
            "\n  The compiler will not warn, the tests will pass and every other"
            "\n  sweep will stay green. See this file's header for the one that"
            "\n  shipped."
        )
        return 1

    print(f"no braceless two-statement bodies in {len(files)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
