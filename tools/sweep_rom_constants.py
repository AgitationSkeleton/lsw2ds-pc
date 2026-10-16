"""Port constants that came from the ROM: do they still agree with it?

WHY THIS EXISTS. The host's "Hats" policy was once found to strip 47 of the 55
hats. `lsw2_customization_is_headwear` tested membership against eight part
ids, and the comment justifying that was careful, honest and correct **for the
evidence available when it was written**. Later work established that the part
id ranges ARE the slots and headwear is 177-231. Nothing rechecked the older
claim, because the two pieces of work were about different halves of the same
customization feature.

None of the existing gates could catch it. The function existed, was called, had
a test, and the test agreed with the code. Only a fact from OUTSIDE the port
made it visibly wrong.

That is the gap this fills. Several port constants are transcriptions of ROM
facts, and those facts now live in regenerable exports - `config/part_slots.json`
and friends, each produced by a tool that reads the image. So the constant and
the fact can be compared, and a constant that drifts from the ROM fails here
instead of waiting for someone to notice.

    python tools/sweep_rom_constants.py

WHAT IT CANNOT DO. It only knows about constants someone has listed below. A
constant nobody registered is exactly as unchecked as before - so adding an
entry is part of transcribing a ROM fact, not an afterthought. The list is short
on purpose: these are the ones whose being wrong would be silent.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def part_slots():
    return json.loads((ROOT / "config" / "part_slots.json")
                      .read_text(encoding="utf-8"))


def slot_bounds(slot: int) -> tuple[int, int]:
    d = part_slots()
    ids = [int(k) for k, v in d["parts"].items() if v["slot"] == slot]
    return min(ids), max(ids)


def pause_row_id(row: int) -> int:
    """Row `row`'s string id, from the table 0x0208E874 copies onto its stack.

    THE TABLE IS 0x020F4CF8 AND THE ROWS START AT +0x08. Rows 0, 1, 3 and 5 read
    straight out of it; row 4's entry is 0x101 "Mos Eisley Cantina" and the
    builder stores a literal 0x2C "Save Game" over it conditionally, so that one
    is deliberately NOT checked here - a gate that asserted the table value for
    row 4 would be asserting the wrong number confidently.
    """
    sys.path.insert(0, str(ROOT / "tools"))
    from armscan import load, BASE  # noqa: E402
    import struct
    d = load()
    return struct.unpack_from("<H", d, 0x020F4CF8 - BASE + 0x08 + row * 2)[0]


def net_messages():
    return json.loads((ROOT / "config" / "net_messages.json")
                      .read_text(encoding="utf-8"))


def imm12(addr: int) -> int:
    """The 12-bit immediate of the load/store instruction at `addr`.

    The head fix's two offsets are both an immediate in one instruction, so the ROM can
    be asked for them instead of them being copied twice. `0x0200668C` is
    `LDRB r0,[r4,#0x28]` - the "this hat covers the head" flag - and
    `0x02006364` is `STRB r1,[r0,#0x6F3]`, the whole of what the re-attach
    marker at 0x02006360 does.
    """
    sys.path.insert(0, str(ROOT / "tools"))
    from armscan import load, BASE  # noqa: E402
    import struct
    d = load()
    ins = struct.unpack_from("<I", d, addr - BASE)[0]
    if (ins >> 26) & 3 != 1:
        raise SystemExit(f"{addr:08X} is not a load/store: {ins:08X}")
    return ins & 0xFFF


def mov_imm(addr: int) -> int:
    """The immediate of the MOV-immediate instruction at `addr`.

    `imm12` above only handles load/store offsets, and the receive pump's
    per-station stride is not one: `0x020B044C` is `MOV r0, #0xA` and the
    stride reaches the address through `MLA r1, r5, r0, r9`. Reading the MOV
    is what makes that constant a transcription the ROM can be asked about
    rather than a number copied out of a disassembly by hand.
    """
    sys.path.insert(0, str(ROOT / "tools"))
    from armscan import load, BASE  # noqa: E402
    import struct
    d = load()
    ins = struct.unpack_from("<I", d, addr - BASE)[0]
    if (ins & 0x0FEF0000) != 0x03A00000:
        raise SystemExit(f"{addr:08X} is not a MOV immediate: {ins:08X}")
    rot = ((ins >> 8) & 0xF) * 2
    imm = ins & 0xFF
    return ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm


# (source file, constant name, what it should be, where that comes from)
#
# The third element is a callable so the expected value is COMPUTED from a ROM
# export rather than written here - a second hand-copy would drift the same way
# the first one did.
CLAIMS = [
    ("port/frontend/customization_policy.cpp", "kHeadwearFirst",
     lambda: slot_bounds(3)[0],
     "config/part_slots.json - the first id in slot 3 (headwear)"),
    ("port/frontend/customization_policy.cpp", "kHeadwearLast",
     lambda: slot_bounds(3)[1],
     "config/part_slots.json - the last id in slot 3"),
    ("port/frontend/viewmask.h", "kLsw2ViewRecords", lambda: 4,
     "four stations; the resolver's loop bound is CMP r8, #4"),
    ("port/frontend/netmsglog.h", "kLsw2MsgStations", lambda: 4,
     "four stations, the same bound the pump walks"),
    ("port/frontend/netmsglog.h", "kLsw2MsgSlotBase", lambda: 0xA3C,
     "the pump reads [record + 0xA3C] at 0x020B043C"),
    ("port/frontend/netmsglog.h", "kLsw2MsgStationStride",
     lambda: mov_imm(0x020B0430),
     "MOV r0, #0x344 at 0x020B0430 - read from the ROM now, not transcribed"),
    ("port/frontend/netmsglog.h", "kLsw2MsgSeqBase", lambda: imm12(0x020B047C),
     "LDRB r0,[r6,#0x40] at 0x020B047C - the last sequence seen"),
    ("port/frontend/netmsglog.h", "kLsw2MsgSeqStride",
     lambda: mov_imm(0x020B044C),
     "MOV r0, #0xA at 0x020B044C, reaching the address through the MLA at "
     "0x020B0450 - ten slots, one sequence byte each"),
    ("port/frontend/customization.h", "kLsw2CustomNone", lambda: 250,
     "config/default_parts.json - the empty-slot sentinel 0x02006220 tests"),
    ("port/frontend/pauselayout.h", "kLsw2PauseIdHostWireless",
     lambda: pause_row_id(1),
     "0x020F4CF8 + 0x0A - row 1's id in the builder's own table"),
    ("port/frontend/pauselayout.h", "kLsw2PauseIdQuitGame",
     lambda: pause_row_id(5),
     "0x020F4CF8 + 0x12 - row 5's id in the builder's own table"),
    ("port/frontend/pauselayout.h", "kLsw2PauseIdExtras",
     lambda: pause_row_id(3),
     "0x020F4CF8 + 0x0E - row 3's id in the builder's own table"),
    ("port/frontend/gamemenu.h", "kLsw2MenuFirstRowY", lambda: 0x3A,
     "ADD r3, r3, #0x3A at 0x0209CB6C - the arena menu's first row Y"),
    ("port/frontend/gamemenu.h", "kLsw2MenuRowPitch", lambda: 0x1C,
     "MOV r4, #0x1C at 0x0209CB00 - the arena menu's row pitch"),
    ("port/frontend/gamemenu.h", "kLsw2MenuCentreX", lambda: 0x80,
     "MOV r2, #0x80 at 0x0209CB68 - both menus centre on 128"),
    # The head fix's four transcriptions. The first two are the SAME ROM fact
    # customization_policy.cpp's kHeadwearFirst/Last already carry, written out
    # a second time in a second file - which is the exact shape of the "Hats"
    # drift described above, so both copies are now pinned to one export.
    ("port/frontend/headfix.h", "kLsw2HeadgearFirst",
     lambda: slot_bounds(3)[0],
     "config/part_slots.json - the first id in slot 3; the same fact "
     "customization_policy.cpp's kHeadwearFirst carries"),
    ("port/frontend/headfix.h", "kLsw2HeadgearLast",
     lambda: slot_bounds(3)[1],
     "config/part_slots.json - the last id in slot 3"),
    ("port/frontend/headfix.h", "kLsw2HeadgearCovers",
     lambda: imm12(0x0200668C),
     "the immediate of LDRB r0,[r4,#0x28] at 0x0200668C - the gear applier "
     "reading the hat's covers flag"),
    ("port/frontend/headfix.h", "kLsw2HeadReattach",
     lambda: imm12(0x02006364),
     "the immediate of STRB r1,[r0,#0x6F3] at 0x02006364, which is all that "
     "0x02006360 does"),
    ("port/frontend/statelog.h", "kLsw2StateCount",
     lambda: json.loads((ROOT / "config" / "game_states.json")
                        .read_text(encoding="utf-8"))["count"],
     "config/game_states.json - the factory's CMP r9, #0x39 bound"),
]

VALUE = re.compile(r"\b{}\s*=\s*(0[xX][0-9a-fA-F]+|\d+)")

# A claim that is not a single constant: the arena predicate must accept
# exactly as many levels as the fork has maps.
#
# WHY IT IS WORTH CHECKING. port/frontend/levels.h names ELEVEN levels "Arena"
# and only two are confirmed - the other nine are asset-name substring matches.
# tools/extract_level_table.py documents that false-positive mode against level
# 14, derived as "Arena (2)" and actually the Cantina hub. So a later reader who
# widened the predicate to match the table would be widening it to noise. The
# authority is the fork's own map list, not the derived names.
ARENA_PRED = re.compile(
    r"lsw2_level_is_arena\(int level\)\s*\{[^}]*?return([^;]*);", re.S)


def check_unlock_lists():
    """The unlock-everything arrays must cover every row of the customize table.

    `0x020EE704` is the Character Customize screen's row list: 8-byte rows,
    byte 0 the kind, byte 1 the part id, then three asset ids. Kind 1 rows carry
    the indices `+0x450C` counts over; kind 2 rows carry actor part ids for
    `+0x44EC`. Kinds 0, 3 and 4 are single rows with id 0 - defaults that need
    no unlock.

    If a row is added or the reading of the table changes, an unlock list that
    no longer covers it leaves a row the player cannot buy - invisible until
    someone opens the panel and finds an item greyed out.
    """
    sys.path.insert(0, str(ROOT / "tools"))
    from armscan import load, BASE  # noqa: E402

    data = load()
    off = 0x020EE704 - BASE
    counts = {}
    for i in range(200):
        kind = data[off + i * 8]
        asset = data[off + i * 8 + 2] | (data[off + i * 8 + 3] << 8)
        if kind > 0x10 or asset == 0 or asset > 0x2000:
            break
        counts[kind] = counts.get(kind, 0) + 1

    src = (ROOT / "port" / "frontend" / "cheats.cpp").read_text(
        encoding="utf-8", errors="replace")
    problems = []
    for name, kind in (("kCustomisePartIds", 2), ("kCustomiseOtherIds", 1)):
        m = re.search(name + r"\[\]\s*=\s*\{([^}]*)\}", src, re.S)
        if not m:
            problems.append(f"{name} not found in cheats.cpp")
            continue
        n = len(re.findall(r"\d+|0x[0-9a-fA-F]+", m.group(1)))
        want = counts.get(kind, 0)
        if n != want:
            problems.append(f"{name} has {n} ids but the customize table has "
                            f"{want} kind-{kind} rows")
    return problems


def check_extras_table():
    """The host's Extras list against the ROM's strings AND the required hidden set.

    `hostsettings.cpp` carries eighteen names with an `offered` flag, and the
    file says why they are one table rather than two: "the rule and the names
    cannot drift apart". That is right, and it leaves the table answerable to
    two outside authorities at once - the string bank it transcribes, and the
    feature's list of eight to hide. Neither is visible from the C.

    The names are strings 0x243-0x254, flag = id - 0x243.
    """
    import json as _json
    src = (ROOT / "port" / "frontend" / "hostsettings.cpp").read_text(
        encoding="utf-8", errors="replace")
    m = re.search(r"const Extra kExtras\[kLsw2ExtrasCount\] = \{(.*?)\n\};",
                  src, re.S)
    if not m:
        return ["kExtras not found in hostsettings.cpp"]
    rows = re.findall(r'\{"([^"]+)",\s*(true|false)\}', m.group(1))

    strings = _json.loads((ROOT / "config" / "strings.json")
                          .read_text(encoding="utf-8"))
    table = strings.get("strings", strings)

    problems = []
    for i, (name, _) in enumerate(rows):
        rom = table.get("0x%03X" % (0x243 + i), "")
        if not rom.lower().startswith(name.lower()):
            problems.append(f"extras[{i}] is {name!r} but string 0x{0x243+i:03X} "
                            f"is {rom!r}")

    # The eight Extras the host must never offer, as hostsettings.h lists them.
    required = {"Invincibility", "Minikit Detector", "Tiny Enemies",
               "Ewok Invasion", "Pink", "Sunglasses", "Droid Invasion",
               "Moustaches"}
    hidden = {n for n, offered in rows if offered == "false"}
    for missing in sorted(required - hidden):
        problems.append(f"the required hidden set has {missing!r} and the host still "
                        f"offers it")
    for extra in sorted(hidden - required):
        problems.append(f"{extra!r} is hidden but the required hidden set does "
                        f"not include it")
    return problems


# The head fix's two derived sets, as headfix.h commits to them. Written here
# rather than read from config/head_coverage.json on purpose: the JSON is
# GENERATED from the ROM by tools/export_head_coverage.py, so comparing the ROM
# against it would compare the ROM with itself and pass no matter what.
EXPECTED_COVERING_HATS = [180, 189, 193, 201, 222, 224, 225, 226, 227]
EXPECTED_AFFECTED = [7, 11, 15, 29, 60, 61, 63, 64, 65]


def check_slot_pitch():
    """The match HUD's row pitch, transcribed from a capture and checked against nothing.

    `arenahud.cpp`'s kSlotPitch is 48 because that is what the recorded arena
    session measured, and config/bounty_hud_layout.json records the same number
    TWICE - `slot_column.pitch` and `bottom_screen_sprites.player_slots.pitch_y`.
    Nothing compared them.

    IT IS LOAD-BEARING, which is what makes it worth pinning rather than just
    tidy. read_rows() identifies which player owns a row with
    `dy % kSlotPitch` and `dy / kSlotPitch`; the composite then blits each
    row's strip from that offset. A pitch that drifted from the capture would
    not fail, it would copy the WRONG STRIPS - and the config file's own note
    says what that looks like: "a port that read the column positionally would
    have shown the wrong player's score strip" for 461 frames of that capture.

    This compares code against CAPTURE DATA, not against a generator's own
    output: bounty_hud_layout.json is a recording of what the DS drew, and
    arenahud.cpp is a hand transcription of it. Those are two independent
    things that can disagree.
    """
    out = []
    src = (ROOT / "port" / "frontend" / "arenahud.cpp").read_text(
        encoding="utf-8", errors="replace")
    m = re.search(r"\bkSlotPitch\s*=\s*(\d+)", src)
    if not m:
        return ["arenahud.cpp: kSlotPitch not found - renamed?"]
    code = int(m.group(1))

    layout = json.loads((ROOT / "config" / "bounty_hud_layout.json")
                        .read_text(encoding="utf-8"))
    a = layout.get("slot_column", {}).get("pitch")
    b = (layout.get("bottom_screen_sprites", {})
               .get("player_slots", {}).get("pitch_y"))
    if a is None or b is None:
        return ["bounty_hud_layout.json: the two slot pitches are not both "
                "present any more"]
    if a != b:
        out.append(f"bounty_hud_layout.json disagrees with itself: "
                   f"slot_column.pitch is {a} but player_slots.pitch_y is {b}")
    if code != a:
        out.append(f"arenahud.cpp: kSlotPitch is {code}, but the capture in "
                   f"bounty_hud_layout.json measured {a} - read_rows() would "
                   f"attribute rows to the wrong players")
    return out


def check_head_coverage():
    """The head fix's affected set, which is five UNPINNED constants deep.

    headfix.h pins four of its numbers here - kLsw2HeadgearFirst, ...Last,
    ...Covers and kLsw2HeadReattach - and leaves five unpinned that decide
    WHICH CHARACTERS the fix applies to:

        kLsw2HeadgearTable   0x020F2B1C
        kLsw2HeadgearStride  0x2C
        kLsw2StockParts      0x020EEBB8
        kLsw2StockStride     5
        kLsw2StockHeadgear   3

    lsw2_character_head_hidden_by_stock walks the stock table with those three
    and asks lsw2_headgear_covers about the result, so a wrong base, stride or
    field offset would silently select the WRONG CHARACTERS - and the feature
    would still look implemented, still compile, and still pass every other
    gate. The visible bug was on Boba and Jango; the real set is nine, so "it
    fixes those two" is not even a usable smoke test.

    The check is STRUCTURAL, and it is the same walk headfix.h says was done by
    hand: derive the set from the ROM using the header's own constants and
    compare it against the nine the header commits to. If any of the five is
    wrong the set comes out different, so one assertion covers all of them.
    """
    src = (ROOT / "port" / "frontend" / "headfix.h").read_text(
        encoding="utf-8", errors="replace")

    def const(name):
        m = re.search(r"\b" + name + r"\s*=\s*(0[xX][0-9a-fA-F]+|\d+)", src)
        return int(m.group(1), 0) if m else None

    names = ("kLsw2HeadgearTable", "kLsw2HeadgearStride", "kLsw2HeadgearFirst",
             "kLsw2HeadgearLast", "kLsw2HeadgearCovers", "kLsw2StockParts",
             "kLsw2StockStride", "kLsw2StockHeadgear")
    vals = {n: const(n) for n in names}
    missing = [n for n, v in vals.items() if v is None]
    if missing:
        return [f"headfix.h: cannot read {', '.join(missing)} - renamed?"]

    sys.path.insert(0, str(ROOT / "tools"))
    from armscan import load, BASE  # noqa: E402
    d = load()

    def u8(addr):
        return d[addr - BASE]

    out = []

    # The covers flag must BE a flag, over the whole table.
    covering = []
    for part in range(vals["kLsw2HeadgearFirst"], vals["kLsw2HeadgearLast"] + 1):
        rec = (vals["kLsw2HeadgearTable"] +
               (part - vals["kLsw2HeadgearFirst"]) * vals["kLsw2HeadgearStride"])
        v = u8(rec + vals["kLsw2HeadgearCovers"])
        if v not in (0, 1):
            out.append(f"headfix.h: headgear {part}'s covers byte is {v}, not a "
                       f"flag - kLsw2HeadgearTable or kLsw2HeadgearStride is "
                       f"wrong")
            return out
        if v:
            covering.append(part)

    if covering != EXPECTED_COVERING_HATS:
        out.append(f"headfix.h: the covering hats are {covering}, but the "
                   f"header commits to {EXPECTED_COVERING_HATS}")

    # ...and walking the stock table with it must give the nine characters.
    affected = []
    for character in range(84):
        stock = u8(vals["kLsw2StockParts"] +
                   character * vals["kLsw2StockStride"] +
                   vals["kLsw2StockHeadgear"])
        if vals["kLsw2HeadgearFirst"] <= stock <= vals["kLsw2HeadgearLast"]:
            rec = (vals["kLsw2HeadgearTable"] +
                   (stock - vals["kLsw2HeadgearFirst"]) *
                   vals["kLsw2HeadgearStride"])
            if u8(rec + vals["kLsw2HeadgearCovers"]):
                affected.append(character)

    if affected != EXPECTED_AFFECTED:
        out.append(f"headfix.h: the head fix's affected set walks out as {affected}, "
                   f"but the header commits to {EXPECTED_AFFECTED} - one of "
                   f"kLsw2StockParts, kLsw2StockStride or kLsw2StockHeadgear "
                   f"is wrong, and the fix is being applied to the wrong "
                   f"characters")
    return out


def check_arena_predicate():
    src = (ROOT / "port" / "frontend" / "cheats.h").read_text(
        encoding="utf-8", errors="replace")
    m = ARENA_PRED.search(src)
    if not m:
        return "lsw2_level_is_arena not found in cheats.h - renamed?"
    accepted = sorted({int(x) for x in re.findall(r"level\s*==\s*(\d+)",
                                                  m.group(1))})
    maps = json.loads((ROOT / "config" / "bounty_match_options.json")
                      .read_text(encoding="utf-8"))["maps"]
    if len(accepted) != len(maps):
        return (f"lsw2_level_is_arena accepts {len(accepted)} level(s) "
                f"{accepted}, but the fork has {len(maps)} arena map(s) "
                f"{[x['key'] for x in maps]}")
    return None


def main() -> int:
    bad = []
    checked = 0
    for rel, name, expect, why in CLAIMS:
        path = ROOT / rel
        if not path.is_file():
            bad.append(f"{rel} does not exist - {name} cannot be checked")
            continue
        m = VALUE.pattern.format(re.escape(name))
        found = re.search(m, path.read_text(encoding="utf-8", errors="replace"))
        if not found:
            bad.append(f"{name} not found in {rel} - renamed or removed?")
            continue
        got = int(found.group(1), 0)
        want = expect()
        checked += 1
        if got != want:
            bad.append(f"{rel}: {name} is {got} ({hex(got)}), the ROM says "
                       f"{want} ({hex(want)})\n      source: {why}")

    arena = check_arena_predicate()
    if arena:
        bad.append(arena)
    else:
        checked += 1

    heads = check_head_coverage()
    bad.extend(heads)
    if not heads:
        checked += 2      # the covering hats, and the characters wearing them

    pitch = check_slot_pitch()
    bad.extend(pitch)
    if not pitch:
        checked += 1

    # The two extras counts are in different headers and are passed to each
    # other across a call boundary; a silent disagreement would write the wrong
    # number of flags. Cheap to check, invisible in C.
    import re as _re
    ca = _re.search(r"kLsw2ExtrasCount\s*=\s*(\d+)",
                    (ROOT / "port" / "frontend" / "hostsettings.h")
                    .read_text(encoding="utf-8"))
    cb = _re.search(r"kLsw2ExtrasEnabledCount\s*=\s*(\d+)",
                    (ROOT / "port" / "frontend" / "cheats.h")
                    .read_text(encoding="utf-8"))
    if not ca or not cb:
        bad.append("could not read both extras counts")
    elif ca.group(1) != cb.group(1):
        bad.append(f"kLsw2ExtrasCount is {ca.group(1)} but "
                   f"kLsw2ExtrasEnabledCount is {cb.group(1)} - "
                   f"extrasapply passes the first to a function bounded by "
                   f"the second")
    else:
        checked += 1

    extras = check_extras_table()
    bad.extend(extras)
    if not extras:
        checked += 2

    unlock = check_unlock_lists()
    bad.extend(unlock)
    if not unlock:
        checked += 2

    if bad:
        print("port constants disagree with the ROM they came from:")
        for line in bad:
            print(f"  {line}")
        print("\n  A constant that drifts from the ROM is the failure the Hats"
              "\n  policy hit: right when written, silently wrong afterwards.")
        return 1

    print(f"all {checked} registered ROM constants match their exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
