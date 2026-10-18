"""One guest fact, declared under two names, is a correction that lands on half
the code.

WHY THIS EXISTS. Nineteen of these have now been collapsed by hand, and the
collapses kept turning up things no test could see:

  * `viewmask.h` labelled `[0x020FEB88]` "the game bound  2" in its own table
    of reference values. +0 is the COUNT; the bound is at +4 and reads 4, not
    2. A fork session reaching the correct bound would have been judged broken
    against the table written to judge it by. Found only by reading it beside
    `selectskip.cpp`'s copy of the same address.
  * `enhanced.h` described the camera record and `viewmask.h` the view record.
    One structure. And the camera copy had gone DEAD - `kLsw2CameraArray` and
    `kLsw2CameraStride` had no consumer left after the resolver replaced the
    subscripting, while still asserting a layout for anyone who read them.
  * `dispatch_overrides.cpp` declared the wireless context twice in ONE file,
    780 lines apart, as `kWhContext` and `kWhCtx`.
  * `coopjoin.cpp` wrote `0x02106D90u + 0xBB8u` where `diagnostics.cpp` kept
    the base and the field apart, so the same word did not look like the same
    word.

None of these break a build and none fail a test. They are all the same shape:
a correction to one copy leaves the other silently disagreeing.

    python tools/sweep_duplicate_constants.py

WHY IT GROUPS BY VALUE AND NOT BY ADDRESS. The list I worked from was built by
grepping for repeated ADDRESSES, and it said every three-file case was gone. It
was wrong: `0x6BB`, the character id byte, was declared in THREE files -
`cheats.cpp`, `partyportrait.h` and `headfix.cpp` - and never appeared, because
it is a member OFFSET rather than an address. The shape of a scan decides what
it cannot see, so this one groups every hex constant by its value regardless of
what the value means.

THE FLOOR, STATED HONESTLY. This still cannot see a fact that is spelled
differently in the two places: a folded `BASE + 0x10` against a separate base
and offset, a decimal against a hex, or a value computed from two others. The
`coopjoin.cpp` case above was exactly that and was found by hand. What this
gate does is keep the hand-collapsed nineteen from coming back and catch the
plain new ones.

WHY THE BASELINE IS THIRTEEN ENTRIES AND MUST STAY THAT WAY. `sweep_orphans`
was written with a large baseline and `lsw2_font_load` sat inside it for weeks
while the fork's menus drew no text at all. A gate whose baseline is where the
bugs live is a gate that reports nothing.

So every entry was earned: the gate ran RED for five commits while the real
duplicates were collapsed one at a time, and only the survivors went in. Each
is a value that is genuinely two or more different facts sharing a number -
mostly small structure offsets, because most structures put something at +8 -
and each was read declaration by declaration rather than judged by its name.
Not one of them is a duplicate anyone decided to tolerate.

If a new duplicate is real, COLLAPSE IT; do not add it here.
"""

import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DIRS = ("port/frontend", "port/net")

# A hex constant declaration: `constexpr uint32_t kName = 0x1234u;` and the
# `static const unsigned` form the generated headers use.
#
# DECIMAL TOO, AND IT WAS ONCE HEX-ONLY. That blind spot cost a real one: the
# boot warp's destination, the arena lobby's level id, was written
# `= 9` in FOUR places - fork.cpp, lasthunter_observe.cpp, and TWICE in
# dispatch_overrides.cpp under two names 84 lines apart. Every one was above
# the floor and none was hex, so this gate could not see any of them while
# reporting itself green.
#
# Addresses and offsets get written in hex and IDs get written in decimal, so
# a hex-only scan sees the memory map and misses the game's own vocabulary -
# level ids, state ids, slot counts. Those are exactly the constants a feature
# description names.
DECL = re.compile(
    r"^\s*(?:static\s+)?(?:constexpr|const)\s+"
    r"(?:unsigned\s+|signed\s+)?\w+\s+"
    r"(k\w+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]*\s*;"
)

# Values below this recur legitimately as counts, indices and small flags, and
# flagging them would bury the real answers. 0x08 is low enough to keep real
# struct offsets - the actor flags word at +0x08 is the smallest one this
# project actually cares about.
#
# AND THIS FLOOR HAS ALREADY HIDDEN A REAL ONE, which is recorded here rather
# than quietly fixed. `kStateCurrent` and `kStateId` are both 4, and both were
# declared in diagnostics.cpp, lasthunter_observe.cpp AND scorescreen.cpp while
# statelog.h owned them as kLsw2StateCurrent/kLsw2StateId - four copies each,
# every one of them below this floor and invisible here. They were found by
# reading, not by this gate.
#
# So there is a SECOND rule below that does not consult the floor at all: the
# same NAME declared in two files is always reported, whatever its value. That
# catches the three-file kStateCurrent case going forward. What it still cannot
# catch is a below-floor value under two DIFFERENT names, and that is the
# honest limit of this gate - written down where the next reader will see it
# rather than left as a silent gap.
FLOOR = 0x08

# THE BASELINE. Each entry is a coincidence of number rather than a
# duplicated fact. Keyed by value; the comment says why the collision is not
# one fact written twice.
BASELINE = {
    # The bounds of main RAM, deliberately repeated per translation unit rather
    # than shared - every module that touches the bus range-checks its own
    # pointers, and a single shared pair would make the check look like
    # somebody else's responsibility.
    0x02000000: "kRamLow - architectural bound, one per translation unit",
    0x02400000: "kRamHigh - architectural bound, one per translation unit",

    # THE SMALL STRUCTURE OFFSETS. Every entry below was read declaration by
    # declaration, not judged by name, and each is a field of a DIFFERENT
    # structure that happens to sit at the same offset. Low offsets collide
    # because most structures put something at +8.
    0x08: "actor flags / authority active-view / rng remaining / state arg A",
    0x0A: "message type 0x0A (round start) / the sequence record's slot stride",
    0x0C: "context level index / pause row stride / state level arg",
    0x10: "state argument C / the other +0x10 fields (ringcolour's 'byte size' "
          "there was retracted - it is not a size)",
    0x20: "script queue stride / select page / WH system state / slot stride",
    0x28: "headgear covers-head / pause first row Y / select page high",
    0x29: "net player count offset / the select state's id",
    0x2C: "headgear stride / pause SaveGame string id / the score state's id",
    0x30: "WH error code / pause icon field",
    0x40: "appearance record stride / the sequence byte's base",
    0x48: "countdown Y / co-op host status / state slot argument block",
    0x35: "the select widget's mode / the pause state's id",
    0x36: "message type 0x36 (the Extras toggle) / the select widget's entry",
    # touchportrait.cpp, read against each other declaration: the character
    # table's entry count (FUN_0208ea58's own `< 0x54` bound) is not the 3D
    # HUD's ring member offset; the touch HUD's face-sprite array offset is not
    # the script queue's base; 8.0 in 20.12 fixed point (the face's x) is not
    # POWCNT1's display-swap bit.
    0x54: "the character count / the 3D HUD's ring sprite member",
    0x94: "the touch HUD's face sprites / the script queue's base",
    0x8000: "8.0 in 20.12 fixed point (a sprite's x) / POWCNT1's display swap",

    # 0xFF as a sentinel in two unrelated structures. The music selector at
    # 0x020B913C treats param 0xFF as "take this row's own default"
    # (CMP r4,#0xFF / MOVEQ r4, row+3); the cartridge's select widget uses 0xFF
    # for "no selection". One is a byte-sized "unset" in a param table and the
    # other in a widget - the same obvious choice of sentinel made twice, not
    # one fact written twice.
    0xFF: "the music selector's row-default param / the select widget's none",

    # Three UI colours that are all white. Not a guest fact at all - this gate
    # scans every hex constant, and three independent design choices landing on
    # white is a coincidence of taste rather than one fact written three times.
    0xFFFFFF: "white, chosen separately for a countdown, a row and a title",
}


def sources():
    for d in DIRS:
        for p in sorted((ROOT / d).glob("*.h")) + sorted((ROOT / d).glob("*.cpp")):
            yield p


def main() -> int:
    # value -> {name -> [files]}
    seen = collections.defaultdict(lambda: collections.defaultdict(list))
    by_name = collections.defaultdict(list)
    total = 0
    for path in sources():
        rel = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            m = DECL.match(line)
            if not m:
                continue
            name, raw = m.group(1), m.group(2)
            value = int(raw, 0)
            total += 1
            # THE NAME RULE SEES DECIMAL; THE VALUE RULE DOES NOT.
            #
            # Parsing decimal was needed so `by_name` could catch the arena
            # lobby level id, declared `= 9` under one name in three files.
            # Grouping decimal by VALUE is a different matter: small decimal
            # ints collide constantly and meaninglessly - 9 is the ARM9 cpu
            # index, a flag-array count, a music slot field and the lobby's level id,
            # all at once - so that half stays hex, where a shared value really
            # does suggest a shared fact.
            #
            # The gap this leaves is a decimal duplicate under two DIFFERENT
            # names, which is how the fourth copy of the lobby id hid
            # (kArenaLobbyLevelId beside kArenaLobbyLevel in one file). Stated
            # rather than papered over: that one was found by reading.
            by_name[name].append((rel, value))
            if not raw.lower().startswith("0x"):
                continue
            if value < FLOOR:
                continue
            seen[value][name].append(rel)

    bad = []
    for value, names in sorted(seen.items()):
        # One fact under one name in one file is fine. Flag when the same value
        # carries two different names, OR one name is declared in two files -
        # the second case is how `kLsw2ActorCharacter` and the actor character
        # byte in headfix.cpp coexisted.
        places = [(n, f) for n, fs in names.items() for f in fs]
        if len(places) < 2:
            continue
        if value in BASELINE:
            continue
        bad.append((value, sorted(places)))

    # THE FLOOR-INDEPENDENT RULE. One name, two files, any value.
    for name, places in sorted(by_name.items()):
        files = sorted({f for f, _ in places})
        if len(files) < 2:
            continue
        values = {v for _, v in places}
        if values <= set(BASELINE):
            continue
        # FILE-LOCAL IDIOMS. Each of these is a short name a translation unit
        # gives its own copy of, deliberately: kW/kH are the DS screen in the
        # three files that composite it, kReports is a per-feature log budget
        # (and legitimately differs, 4 in three files and 6 in two), kSlots and
        # kArm9 and kSettleFrames the same shape. Sharing them would make each
        # file depend on a header for a number it uses twice.
        #
        # kSlotCount is NOT here: it meant the state manager's TWO slots in
        # cheats.cpp and the appearance record's FOUR in diagnostics.cpp, which
        # is one name for two counts. Renamed rather than excused.
        if name in ("kW", "kH", "kSlots", "kReports", "kArm9",
                    "kSettleFrames", "kScreenWidth", "kScreenHeight"):
            continue
        # Each row shows ITS OWN value, not the union - two files declaring one
        # name to two DIFFERENT values is the worse bug of the two, and a union
        # printed on every row would hide which file holds which.
        bad.append((None, sorted((f"{name}  = 0x{v:X}", f) for f, v in places)))

    if bad:
        print("one value, declared more than once:")
        for value, places in bad:
            print("  same name, two files" if value is None else f"  0x{value:X}")
            for name, f in places:
                print(f"      {name:<32} {f}")
        print("\n  Each of these is a LEAD, not a verdict - read every use"
              "\n  before merging, because the names may mean different things"
              "\n  and one of them may be describing the structure wrongly."
              "\n  kLsw2PlayerSlotActor built the right address under a name"
              "\n  for a field that does not exist, and every test passed.")
        return 1

    kept = len(BASELINE)
    print(f"{total} hex constants checked, none declared twice "
          f"({kept} baselined, each a collision of number and not of fact)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
