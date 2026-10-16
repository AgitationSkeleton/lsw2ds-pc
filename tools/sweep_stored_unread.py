"""A value that is stored and never read is a measurement nobody can see.

WHY THIS EXISTS. `menuhost.cpp` kept every menu event in a file-static called
`g_last` and nothing ever read it, so every Activated and ValueChanged the
player generated was dropped on the floor. All of the host settings' in-game UI moved its
cursor and did nothing. The code that produced the value was correct; the code
that consumed it did not exist.

That is not the same question `sweep_orphans` asks (does this DECLARATION have
any use) or the one `sweep_test_only.py` asks (is this FUNCTION only reached by
tests). It is about state: a variable the port writes and never consults.

    python tools/sweep_stored_unread.py

WHAT IT FOUND on the pass that introduced it, all of them real:

  * `customization.cpp`'s `g_dirty` - set on every adopted change, cleared on
    load and by an exported `mark_clean()`, and asked by nothing. The settings
    writer stored all six customize keys on every save and then marked clean a
    flag no decision depended on.
  * `frontend.cpp`'s `g_debug_key_events` and `g_debug_touch_events` - the two
    counters that exist so a driving script can confirm its injected presses
    landed. `nds_frontend_input_debug_state` says exactly that in its own
    comment and did not fill either field.
  * `dispatch_overrides.cpp`'s `g_cache_hits` and `g_cache_misses` - the string
    cache could have been missing every single time and looked identical from
    outside.

Chasing those turned up three more of the same shape one level along:
`lsw2_hle_hits`, `lsw2_hle_step_hits` and `lsw2_hle_step_stalls` are exported
accessors that NOTHING called - not production, not even a test - under a
comment reading "reported rather than assumed, because 'registered' and
'reached' are different claims". They were not reported. All six counters now
land in one line at shutdown.

WHY THE CLASSIFIER IS THE WHOLE DIFFICULTY. Deciding whether an occurrence is a
read or a write by looking at the text around it goes wrong in two ways that
both report healthy code as broken, and the first version of this hit both:

  * `return g_x;` matches the declaration shape exactly - `return` reads as the
    type - which filed every accessor body as a declaration rather than a read
    and reported four correctly-wired mirrors as write-only.
  * `++g_settled >= kSettleFrames` is a write AND a read: the value feeds a
    comparison. Counting increments as writes alone reported a working settle
    timer as dead state.

Nine reported entries became five once both were fixed, and the five were real.
A sweep that cries wolf gets switched off, so the arithmetic matters more than
the idea.

WHAT IT CANNOT DO. See a value read in ANOTHER translation unit. It only looks
at file-static names - the `g_` prefix this port uses for them - inside the file
that declares them, which is the scope those actually have. A non-static global
read from elsewhere is out of scope by construction, and `sweep_orphans` covers
that side.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

BS = chr(92)          # a backslash, BUILT rather than typed: this project has
                      # lost escapes to heredocs enough times to stop typing
                      # them in anything that might travel through a shell.


def strip(text: str) -> str:
    """Comments and literals out. A `g_x` inside a string is not a use."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:[^"' + BS + BS + r']|' + BS + BS + r'.)*"', '""', text)
    text = re.sub(r"'(?:[^'" + BS + BS + r"]|" + BS + BS + r".)*'", "''", text)
    return text


DECL = re.compile(
    r"^(?:static\s+)?(?:constexpr\s+)?(?:const\s+)?"
    r"(?:unsigned\s+|signed\s+|struct\s+|enum\s+)?"
    r"[A-Za-z_][\w:<>,\s*&]*?[\s*&]"
    r"(g_[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|;|\{)", re.M)

WRITE = r"(?:=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=)"

# Statements that begin with one of these are not declarations, whatever they
# look like. `return g_x;` is the one that matters and it cost four false
# reports before it was noticed.
KEYWORDS = {"return", "if", "while", "for", "switch", "case", "else", "do",
            "sizeof", "delete", "throw"}

# Written and not read on purpose, with the reason. Empty today, and meant to
# stay short: unlike an unused accessor, dead STATE is nearly always either a
# missing consumer or a leftover, and both want fixing rather than listing.
ALLOWED: dict[str, str] = {}


def classify(text: str, name: str) -> tuple[int, int, int]:
    reads = writes = decls = 0
    for m in re.finditer(r"(^|[^\w])" + re.escape(name) + r"(?![\w])", text):
        pos = m.end()
        ls = text.rfind("\n", 0, m.start()) + 1
        le = text.find("\n", pos)
        line = text[ls:le if le >= 0 else len(text)]
        stripped = line.lstrip()
        after = text[pos:pos + 40]
        before = text[max(0, m.start() - 3):m.start() + 1]

        kw = re.match(r"(\w+)", stripped)
        dm = DECL.match(stripped)
        if dm and dm.group(1) == name and (not kw or
                                           kw.group(1) not in KEYWORDS):
            decls += 1
            continue

        if re.match(r"\s*(?:\[[^\]]*\]\s*)*" + WRITE, after):
            writes += 1
            continue

        # An increment WRITES, and it also READS unless the whole statement is
        # the increment. `++g_settled >= kSettleFrames` feeds a comparison.
        if re.match(r"\s*(?:\+\+|--)", after) or \
           re.search(r"(?:\+\+|--)\s*$", before):
            writes += 1
            if not re.match(r"^\s*(?:\+\+|--)?\s*" + re.escape(name) +
                            r"\s*(?:\+\+|--)?\s*;\s*$", stripped):
                reads += 1
            continue

        reads += 1
    return reads, writes, decls


def main() -> int:
    bad, cleared, scanned = [], set(), 0
    for directory in DIRS:
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.cpp")):
            if path.name.startswith("test_"):
                continue
            text = strip(path.read_text(encoding="utf-8", errors="replace"))
            for name in sorted({m.group(1) for m in DECL.finditer(text)}):
                scanned += 1
                reads, writes, _ = classify(text, name)
                if writes == 0 or reads > 0:
                    continue
                key = f"{path.name}:{name}"
                if key in ALLOWED:
                    cleared.add(key)
                    continue
                bad.append(f"{name} ({path.name}) is written {writes}x and "
                           f"never read")

    for stale in sorted(set(ALLOWED) - cleared):
        bad.append(f"ALLOWED lists {stale!r}, which is now read or no longer "
                   f"exists - take it off the list")

    if bad:
        print("stored and never read:")
        for line in bad:
            print(f"  {line}")
        print("\n  Each is a LEAD - read it. g_last looked like an idle field"
              "\n  and it was every menu press in the fork going nowhere.")
        return 1

    print(f"{scanned} file statics scanned; every one that is written is also "
          f"read ({len(cleared)} cleared by hand)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
