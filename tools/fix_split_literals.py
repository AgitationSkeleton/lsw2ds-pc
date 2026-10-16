"""Repair C string literals that a shell heredoc split across two lines.

THE TRAP. Writing C through a Python heredoc in this project mangles backslash
escapes: a `\\n` inside a string literal arrives as a real newline, so

    fprintf(stderr, "lsw2: something went wrong\\n");

becomes

    fprintf(stderr, "lsw2: something went wrong
    ");

which is a compile error forty lines from anything that looks related. It has
broken the build four separate times.

THE REAL FIX IS NOT TO DO IT - use the editor tools for anything containing an
escape. This exists because the recovery was being retyped each time, and a
five-hundred-object rebuild is a bad place to discover the same mistake twice.

The repair is unambiguous: a line whose double quotes are unbalanced, followed
by a line that starts with a quote, is one literal that was cut in half at an
escape. Joining them with a real backslash-n restores exactly what was meant.
Anything else is left alone.
"""

from __future__ import annotations

import argparse
import pathlib

ESCAPED_NEWLINE = chr(92) + "n"


def repair(text: str) -> tuple[str, int]:
    lines = text.split("\n")
    out: list[str] = []
    i = fixed = 0
    while i < len(lines):
        cur = lines[i]
        if (cur.count('"') % 2 == 1
                and i + 1 < len(lines)
                and lines[i + 1].lstrip().startswith('"')):
            out.append(cur + ESCAPED_NEWLINE + lines[i + 1].lstrip())
            fixed += 1
            i += 2
            continue
        out.append(cur)
        i += 1
    return "\n".join(out), fixed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    total = 0
    for name in a.files:
        p = pathlib.Path(name)
        text = p.read_text(encoding="utf-8")
        fixed_text, n = repair(text)
        if n:
            p.write_text(fixed_text, encoding="utf-8")
            print("%s: rejoined %d split literal(s)" % (p.name, n))
            total += n
    if not total:
        print("no split literals found")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
