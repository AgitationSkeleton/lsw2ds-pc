"""A member written and never read, one level up from a file static.

WHY THIS EXISTS. `sweep_stored_unread.py` asks whether a FILE STATIC is ever
read, which is the shape `menuhost.cpp`'s `g_last` had - every menu event the
player generated stored in it and nothing looking. State kept in a struct or a
class member hides exactly the same way and that sweep cannot see it, because
the name is qualified and the declaration is inside a body.

    python tools/sweep_member_unread.py

WHAT IT FOUND on the pass that introduced it: `Lsw2ForkSession::pick_rows_`,
assigned the page's entry count immediately after the Character menu was built
and read by nothing. It was not a bug on its own - the Activated handler
recomputes the count from `pick_.page`, which cannot change without rebuilding
the menu - but it was a second copy of a number that already had an owner, and
`customization.cpp` states the rule this project settled on for that: two paths
to the same state is how they drift apart, and the surviving one is the one
that is exercised. It has been removed rather than given a reader.

WHAT COUNTS AS A READ. Any mention that is not the declaration, not an
assignment to it, and not a bare increment statement. `.member`, `->member` and
the unqualified name inside the owning class's own methods all count, which is
why the search is over the whole production blob rather than one file: a
private member named `pick_rows_` is written unqualified from inside the class
and read qualified from outside, and looking in only one place would miss
whichever half was elsewhere.

THE PRICE OF THAT BREADTH is that two members with the same name in different
structs are one name here. It makes the sweep quieter rather than noisier - a
read of either clears both - so what survives is worth reading. The alternative,
resolving each mention to its declaring type, needs a parser this does not have
and would fail differently.

WHAT IT CANNOT DO. Tell a redundant copy from a missing consumer. `pick_rows_`
was the first; a field that SHOULD have had a reader looks identical, and only
reading the code says which. That is the same limit every sweep here has, and
the reason ALLOWED entries carry their reason rather than just a name.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

BS = chr(92)          # built, not typed - heredocs eat these

MEMBER = re.compile(
    r"^\s*(?:mutable\s+)?(?:static\s+)?(?:const\s+)?"
    r"(?:unsigned\s+|signed\s+|struct\s+|enum\s+)?"
    r"[A-Za-z_][\w:<>,\s*&]*?[\s*&]"
    r"([a-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;", re.M)

WRITE = r"(?:=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=)"

KEYWORDS = {"return", "if", "while", "for", "switch", "case", "else", "do",
            "sizeof", "delete", "throw", "struct", "class", "public",
            "private", "protected", "using", "typedef", "namespace"}

# Hand-checked and cleared, with the reason.
ALLOWED = {
    # A HARDWARE DECODER IS ALLOWED TO BE COMPLETE. objsprite.h decodes an OAM
    # entry field by field, and its own header explains why it exists at all:
    # "an OAM decode that is subtly wrong does not fail - it draws the wrong
    # tile at the wrong place in the wrong colours". Both fields are asserted
    # by test_objsprite against a reference.
    #
    # `priority` is not needed by any consumer, for a stated reason: arenahud
    # composites from the FRAMEBUFFER using the sprite rectangles, so draw order
    # is already resolved in the pixels it copies. slotcolour rewrites attribute
    # 2's palette bits in the raw halfword and says so: "everything else in that
    # halfword - the tile number and the priority - is left exactly as it was".
    #
    # `tile` USED to sit here with the same reasoning ("would only matter to
    # something reading OBJ character memory directly, which nothing does").
    # Since 2026-09-12 something does: arenahud locates the DS signal meter by
    # its OAM fingerprint - the one 16x16, 16-colour, TILE 0, palette 0 sprite -
    # so the field is read and the gate took the entry off, as its own message
    # says to.
    ("Lsw2ObjSprite", "priority"): "draw order is already resolved in the "
                                   "pixels arenahud copies",
}


def strip(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:[^"' + BS + BS + r']|' + BS + BS + r'.)*"', '""', text)
    text = re.sub(r"'(?:[^'" + BS + BS + r"]|" + BS + BS + r".)*'", "''", text)
    return text


def bodies(text: str):
    """(name, body) for every struct or class definition in `text`."""
    out = []
    for m in re.finditer(r"\b(?:struct|class)\s+(\w+)[^;{]*\{", text):
        start = m.end()
        depth, i = 1, start
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        out.append((m.group(1), text[start:i - 1]))
    return out


def classify(text: str, name: str) -> tuple[int, int]:
    reads = writes = 0
    pattern = re.compile(r"(?:(?:\.|->)\s*)?\b" + re.escape(name) + r"\b")
    for m in pattern.finditer(text):
        after = text[m.end():m.end() + 40]
        before = text[max(0, m.start() - 3):m.start()]
        ls = text.rfind("\n", 0, m.start()) + 1
        le = text.find("\n", m.end())
        stripped = text[ls:le if le >= 0 else len(text)].lstrip()

        dm = MEMBER.match(stripped)
        kw = re.match(r"(\w+)", stripped)
        if dm and dm.group(1) == name and (not kw or
                                           kw.group(1) not in KEYWORDS):
            continue                                   # the declaration

        if re.match(r"\s*(?:\[[^\]]*\]\s*)*" + WRITE, after):
            writes += 1
            continue

        # An increment writes, and also reads unless the statement is only the
        # increment. Same rule, and the same reason, as sweep_stored_unread.
        if re.match(r"\s*(?:\+\+|--)", after) or \
           re.search(r"(?:\+\+|--)\s*$", before):
            writes += 1
            if not re.match(r"^\s*(?:\+\+|--)?\s*[\w.>-]*" + re.escape(name) +
                            r"\s*(?:\+\+|--)?\s*;\s*$", stripped):
                reads += 1
            continue

        reads += 1
    return reads, writes


def main() -> int:
    production: dict[str, str] = {}
    for directory in DIRS:
        if not directory.is_dir():
            continue
        for path in sorted(list(directory.glob("*.cpp")) +
                           list(directory.glob("*.h"))):
            if path.name.startswith("test_"):
                continue
            production[path.name] = strip(
                path.read_text(encoding="utf-8", errors="replace"))

    if not production:
        print("no sources found under port/frontend or port/net")
        return 1

    blob = "\n".join(production.values())

    bad, cleared, seen, scanned = [], set(), set(), 0
    for fname, text in production.items():
        if not fname.endswith(".h"):
            continue
        for struct, body in bodies(text):
            for m in MEMBER.finditer(body):
                name = m.group(1)
                if name in KEYWORDS or name in seen:
                    continue
                seen.add(name)
                scanned += 1
                reads, writes = classify(blob, name)
                if writes == 0 or reads > 0:
                    continue
                if (struct, name) in ALLOWED:
                    cleared.add((struct, name))
                    continue
                bad.append(f"{struct}::{name} ({fname}) is written {writes}x "
                           f"and never read")

    for stale in sorted(set(ALLOWED) - cleared):
        bad.append(f"ALLOWED lists {stale}, which is now read or no longer "
                   f"exists - take it off the list")

    if bad:
        print("members stored and never read:")
        for line in bad:
            print(f"  {line}")
        print("\n  Each is a LEAD - read it. A redundant copy and a missing"
              "\n  consumer look identical from here, and only one is fine.")
        return 1

    print(f"{scanned} members scanned; every one that is written is also read "
          f"({len(cleared)} cleared by hand)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
