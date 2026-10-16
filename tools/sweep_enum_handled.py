"""An enum value nothing handles is a menu option that does nothing.

WHY THIS EXISTS. `sweep_setting_consumers.py` asks whether a SETTING has a
consumer, and a two-value setting can pass it while only one of the two values
does anything: the field is read, so the sweep is satisfied. To the player that
is a control with a dead position, which is the failure this project keeps
finding under different names.

`Lsw2HostMusic::Random` is the live example. `Lsw2HostSettings::music` is read,
persisted, sent over the wire and printed - and `Random` appears nowhere in
production, because nothing selects a random battle track. The setting sweep
lists `music` as PENDING on the strength of a hand-written clause; this one
reaches the same conclusion mechanically, from the other direction.

    python tools/sweep_enum_handled.py

WHAT IT CHECKS. Every `enum class` value declared in a header under
`port/frontend` or `port/net` must appear qualified - `Enum::Value` - somewhere
in production. A value that appears only in its own declaration is one nothing
compares against, switches on, or assigns.

WHAT IT CANNOT DO. Recognise the COMPLEMENT of a tested value. A two-value enum
tested as `x == Enum::A` handles `Enum::B` completely and correctly without ever
naming it (Lsw2HostControls::Tank was cleared that way until fork.cpp named
it). Distinguishing that from a genuinely dead value needs to know whether
the enum has exactly two values and whether the test is exhaustive, and getting
that wrong in the reporting direction is what switches a sweep off - so it is
left to the ALLOWED list, where a person states it once.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

BS = chr(92)

# Hand-checked and cleared, with the reason.
ALLOWED = {
    # A PHASE THIS LAYER DOES NOT HAVE. Lsw2NetState mirrors part of
    # WH_SYSSTATE, and Lsw2LockstepSession::seat() goes straight to Ready
    # because the transport is already connected by the time a station is
    # seated - there is no asynchronous connect at this level. The fork
    # session's own Lsw2SessionState::Joining is the state that models it, and
    # that one is consumed all over ui.cpp and forksession.cpp.
    ("Lsw2NetState", "Connecting"):
        "the transport is connected before seat(); Lsw2SessionState::Joining "
        "is where that phase actually lives",

    # THE ONE THAT IS REALLY MISSING, and it is deliberately listed here rather
    # than reported, because sweep_setting_consumers.py already owns it: its
    # PENDING entry for `music` carries the full reason - the ROM does not
    # encode which of the 29 slots are battle tracks, diagnostics.cpp's own
    # legend says so, and config/sound_call_sites.json recorded a capture with
    # "NO combat, damage or death sound present". Two gates reporting one gap
    # twice would mean fixing it needs two edits and forgetting it needs none.
}


def strip(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:[^"' + BS + BS + r']|' + BS + BS + r'.)*"', '""', text)


def enums(text: str):
    """(name, [values]) for each `enum class` or `enum struct`."""
    out = []
    for m in re.finditer(r"\benum\s+(?:class|struct)\s+(\w+)[^{]*\{([^}]*)\}",
                         text):
        values = []
        for part in m.group(2).split(","):
            v = re.match(r"\s*([A-Z]\w*)", part)
            if v:
                values.append(v.group(1))
        out.append((m.group(1), values))
    return out


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

    bad, cleared, scanned = [], set(), 0
    for fname, text in production.items():
        if not fname.endswith(".h"):
            continue
        for name, values in enums(text):
            for value in values:
                scanned += 1
                # The declaration never names the value qualified, so any
                # `Enum::Value` in the blob is a use.
                if re.search(re.escape(name + "::" + value), blob):
                    continue
                if (name, value) in ALLOWED:
                    cleared.add((name, value))
                    continue
                bad.append(f"{name}::{value} ({fname}) is declared and never "
                           f"handled in production")

    for stale in sorted(set(ALLOWED) - cleared):
        bad.append(f"ALLOWED lists {stale}, which is now handled or no longer "
                   f"exists - take it off the list")

    if bad:
        print("enum values nothing handles:")
        for line in bad:
            print(f"  {line}")
        print("\n  A setting can pass sweep_setting_consumers with one of its"
              "\n  positions dead: the FIELD is read, so that sweep is happy.")
        return 1

    print(f"{scanned} enum values scanned; every one is handled "
          f"({len(cleared)} cleared by hand)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
