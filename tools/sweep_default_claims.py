"""A comment that says "off by default" above a default of true.

WHY THIS EXISTS. Stale comments have now cost this project five corrections in
two days - a policy said a handshake did not exist when it did, a race was
called unmeasured in two files after being measured, a header called a
rectangle unknown after it was found. Most of that is semantic and no tool will
catch it.

One slice is mechanical, and it is the slice I caused myself. `ui.h` said "OFF
BY DEFAULT because nobody knows where it is" three lines above
`hud_ping_show = true`, because the default was changed and the paragraph above
it was not. A reader meets the comment first and believes it.

    python tools/sweep_default_claims.py

WHAT IT CHECKS. Every `bool <name> = true;` or `= false;` in the settings
headers, against the comment block immediately above it. A block that says the
setting is off or disabled by default must not sit over a `true`, and one that
says on or enabled must not sit over a `false`.

WHY ONLY BOOLS, AND ONLY THESE HEADERS. This is the shape that actually
happened. A wider rule - every comment against every constant - needs to
understand what the prose is claiming, which is the semantic half that cannot
be automated, and a gate that guesses at meaning reports working code as broken.
The narrow version is worth having precisely because it is not guessing.

WHAT IT CANNOT DO. Notice a comment that is stale in any other way. That
remains a reading job, and the way to do it is the one that found all five:
grep for the words a stale claim uses - "not yet", "unmeasured", "unverified",
"is a guess", "until X lands" - and read each hit against what the code does
now.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADERS = [ROOT / "port" / "frontend" / "ui.h",
           ROOT / "port" / "frontend" / "hostsettings.h"]

# Phrases that claim a default, and which value each implies.
SAYS_OFF = re.compile(r"\b(off by default|disabled by default|default(s|ed)? "
                      r"(to )?off|starts (unticked|off|disabled))\b", re.I)
SAYS_ON = re.compile(r"\b(on by default|enabled by default|default(s|ed)? "
                     r"(to )?on|starts (ticked|on|enabled))\b", re.I)

BOOL_FIELD = re.compile(r"^[ \t]*bool[ \t]+(\w+)[ \t]*=[ \t]*(true|false)[ \t]*;",
                        re.M)


def comment_above(text: str, at: int) -> str:
    """The contiguous comment block immediately preceding `at`.

    Walks back over blank lines and comment lines and stops at the first line
    that is neither - so it reads the block a reader would take as belonging to
    this field, and no further.
    """
    lines = text[:at].splitlines()
    out = []
    for line in reversed(lines):
        s = line.strip()
        if not s:
            if out:
                break
            continue
        if s.startswith("/*") or s.startswith("*") or s.startswith("//") \
                or s.endswith("*/"):
            out.append(s)
            if s.startswith("/*"):
                break
            continue
        break
    return " ".join(reversed(out))


def main() -> int:
    bad, checked = [], 0
    for path in HEADERS:
        if not path.is_file():
            print(f"missing {path}")
            return 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in BOOL_FIELD.finditer(text):
            name, value = m.group(1), m.group(2)
            block = comment_above(text, m.start())
            if not block:
                continue
            checked += 1
            if value == "true" and SAYS_OFF.search(block) \
                    and not SAYS_ON.search(block):
                bad.append(f"{path.name}: {name} defaults to TRUE under a "
                           f"comment saying it is off by default")
            if value == "false" and SAYS_ON.search(block) \
                    and not SAYS_OFF.search(block):
                bad.append(f"{path.name}: {name} defaults to FALSE under a "
                           f"comment saying it is on by default")

    if bad:
        print("settings whose comment contradicts their default:")
        for line in bad:
            print(f"  {line}")
        print("\n  The comment is what a reader meets first, so a stale one is"
              "\n  worse than none. Change both or neither.")
        return 1

    print(f"{checked} bool settings carry a comment; none contradicts its "
          f"default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
