"""Build the warp table: every level id, with a name derived from the ROM.

WHERE THE STRUCTURE COMES FROM, all previously established by earlier reverse
engineering and re-read here rather than trusted:

    0x020F55C4   level records, stride 28, records 0..73
    0x020FF674   permutation: level id -> record index
    0x020FF54C   lighting rig per level id, 73 entries
    0x0210930C   the global holding the CURRENT level id

Three levels were named by prior reverse engineering, and they are exactly the
Bounty Hunter maps:

    id 9  record 1   Cantina Arena Lobby
    id 4  record 53  Jabba's Tower
    id 5  record 56  Mos Eisley Streets

THE OTHER SEVENTY ARE NOT NAMED ANYWHERE, and inventing labels for them would be
guessing dressed as data. So each level's name is DERIVED from its own content:
the record points at bsp / script / texture assets, those assets carry names
lifted out of their Nitro containers, and those names are the game's own
vocabulary for the place - "BattleArena1_MaiD", "Endor_Sky_cmp4".

The output therefore distinguishes three confidence levels, and the UI shows
which is which rather than presenting a guess as a fact:

    confirmed   named by prior reverse engineering
    derived     a label built from the level's own asset names
    unknown     nothing legible; shown as "Level NN"

Usage:  python tools/extract_level_table.py [--out port/frontend/levels.h]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]

RECORDS_ADDR = 0x020F55C4
RECORD_STRIDE = 28
RECORD_COUNT = 74
PERM_ADDR = 0x020FF674
LEVEL_COUNT = 73
IMAGE_BASE = 0x02000000

# Named by prior reverse engineering, not by this script.
CONFIRMED = {
    9: "Cantina Arena Lobby",
    4: "Jabba's Tower (Arena)",
    5: "Mos Eisley Streets (Arena)",
    # Confirmed by OBSERVATION rather than by prior reverse engineering: with a
    # save loaded, the cheats tab read index 14 while the game was visibly in
    # the Cantina hub, and the level's mode class is 0x10 (a hub, not an
    # arena). Its derived name was "Arena (2)", which is exactly the kind of
    # plausible-but-wrong label the derivation is expected to produce.
    14: "Mos Eisley Cantina (Main)",
}

# Asset-name fragments that identify a place, longest first so "deathstar"
# beats "death". Kept explicit rather than clever: a wrong mapping here would
# put a confident wrong name in front of the player.
PLACES = [
    ("battlearena", "Battle Arena"), ("arena", "Arena"),
    ("cantina", "Cantina"), ("moseisley", "Mos Eisley"), ("mos", "Mos Eisley"),
    ("jabba", "Jabba's Palace"), ("tatooine", "Tatooine"),
    ("sandcrawler", "Sandcrawler"), ("deathstar", "Death Star"),
    ("hoth", "Hoth"), ("echo", "Echo Base"), ("dagobah", "Dagobah"),
    ("bespin", "Bespin"), ("cloud", "Cloud City"), ("carbon", "Carbon Freezing"),
    ("endor", "Endor"), ("ewok", "Endor"), ("falcon", "Millennium Falcon"),
    ("rebel", "Rebel Base"), ("yavin", "Yavin"), ("hangar", "Hangar"),
    ("sarlacc", "Great Pit of Carkoon"), ("skiff", "Desert Skiff"),
    ("speeder", "Speeder Chase"), ("tantive", "Tantive IV"),
]


def rom_bytes():
    roms = sorted(ROOT.glob("rom/*.nds"))
    if not roms:
        raise SystemExit("no ROM in rom/")
    return roms[0].read_bytes()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="port/frontend/levels.h")
    args = ap.parse_args()

    image = (ROOT / "out/raw/arm9_section_0_02000000.bin").read_bytes()
    catalog = json.loads(
        (ROOT / "out/raw/archive/catalog.json").read_text(encoding="utf-8"))
    entries = catalog["entries"]

    def at(addr):
        return addr - IMAGE_BASE

    # THE PERMUTATION HOLDS POINTERS, NOT INDICES. Each entry is the address of
    # the level's record, so the index is (ptr - records_base) / stride.
    # Checked against the confirmed names: [9] = 0x020F55E0 = record 1 (Cantina Arena
    # Lobby), [4] = 0x020F5B90 = record 53 (Jabba's Tower).
    ptrs = struct.unpack_from("<%dI" % LEVEL_COUNT, image, at(PERM_ADDR))
    perm = []
    for ptr in ptrs:
        delta = ptr - RECORDS_ADDR
        perm.append(delta // RECORD_STRIDE
                    if ptr >= RECORDS_ADDR and delta % RECORD_STRIDE == 0
                    else -1)

    # Asset names, lowercased once.
    names = {}
    for e in entries:
        joined = " ".join(str(n) for n in (e.get("names") or []))
        if joined:
            names[e["asset_id"]] = joined.lower()

    rows = []
    for level_id in range(LEVEL_COUNT):
        rec = perm[level_id]
        label, source = None, "unknown"

        if level_id in CONFIRMED:
            label, source = CONFIRMED[level_id], "confirmed"
        elif 0 <= rec < RECORD_COUNT:
            off = at(RECORDS_ADDR) + rec * RECORD_STRIDE
            fields = struct.unpack_from("<14H", image, off)
            # Any field that looks like an asset id, checked against the catalog
            # rather than assumed to be at a fixed offset.
            hay = " ".join(names.get(f, "") for f in fields if f < len(entries))
            if hay:
                for frag, pretty in PLACES:
                    if frag in hay:
                        label, source = pretty, "derived"
                        break
        if label is None:
            label, source = "Level %d" % level_id, "unknown"
        rows.append((level_id, rec, label, source))

    # Disambiguate repeats so the dropdown never shows the same text twice.
    seen = {}
    final = []
    for level_id, rec, label, source in rows:
        if source != "unknown":
            seen[label] = seen.get(label, 0) + 1
            if seen[label] > 1:
                label = "%s (%d)" % (label, seen[label])
        final.append((level_id, rec, label, source))

    out = ROOT / args.out
    lines = [
        "/* Generated by tools/extract_level_table.py - do not edit.",
        " *",
        " * level id -> record index -> a name derived from the level's own",
        " * assets. `source` says how much to trust each one:",
        " *   confirmed  named by prior reverse engineering",
        " *   derived    built from the level's own asset names",
        " *   unknown    nothing legible",
        " */",
        "#pragma once",
        "",
        "struct Lsw2Level { int id; int record; const char* name; const char* source; };",
        "",
        "static const Lsw2Level kLsw2Levels[] = {",
    ]
    for level_id, rec, label, source in final:
        lines.append('    {%2d, %2d, "%s", "%s"},' % (level_id, rec, label, source))
    lines.append("};")
    lines.append("static const int kLsw2LevelCount = %d;" % len(final))
    lines.append("")

    # THE BOOT WARP'S DESTINATION, emitted rather than hand-copied.
    #
    # The port boots straight into the Mos Eisley Cantina arena lobby map.
    # That level id was written out FOUR times by hand - fork.cpp,
    # lasthunter_observe.cpp, and TWICE in dispatch_overrides.cpp under two
    # names (kArenaLobbyLevel and kArenaLobbyLevelId, 84 lines apart). Warping
    # to the wrong level does not fail, it puts the player somewhere else and
    # every other lobby feature then operates on the wrong room.
    #
    # It is derived from the table above by NAME, so a re-extraction that
    # renumbered the levels would move this with them instead of leaving four
    # copies of a stale number behind.
    lobby = [lv for lv, _, label, _ in final if label == "Cantina Arena Lobby"]
    if len(lobby) != 1:
        raise SystemExit("expected exactly one 'Cantina Arena Lobby' row, "
                         "found %d - the lobby destination cannot be emitted"
                         % len(lobby))
    lines.append("/* The boot warp's destination, by name from the table above. */")
    lines.append("static const int kLsw2ArenaLobbyLevel = %d;" % lobby[0])
    lines.append("")
    lines.append("/* WHERE THE LIVE LEVEL INDEX ACTUALLY IS.")
    lines.append(" *")
    lines.append(" * NOT at 0x0210930C. That was written down here as \"the global")
    lines.append(" * holding the current level id\" and it is neither: 0x0210930C is a")
    lines.append(" * STRUCT BASE, and the index is a byte two hops away from it.")
    lines.append(" * The chain, from the disassembly -")
    lines.append(" *")
    lines.append(" *     0x02038154  ldr r0,[pc,#0x170]   -> 0x0210930C")
    lines.append(" *     0x02038158  ldr r0,[r0,#0x68]")
    lines.append(" *")
    lines.append(" * and the index sits at +0x479 in the object that yields:")
    lines.append(" *")
    lines.append(" *     index = *(u8*)( *(u32*)(0x0210930C + 0x68) + 0x479 )")
    lines.append(" *")
    lines.append(" * Verified against the running game: reading it in the Cantina hub")
    lines.append(" * gives 14, whose permutation entry has mode class 0x10.")
    lines.append(" *")
    lines.append(" * Writing the index does NOT load a level - it is what the game")
    lines.append(" * reads, not the thing that acts on it. A warp needs the load path,")
    lines.append(" * and THAT SENTENCE USED TO END \"which is not located yet\".")
    lines.append(" * It was located: lsw2_cheats_warp writes a request into the")
    lines.append(" * state manager's own block, as 0x0205EC4C does, and the")
    lines.append(" * game's transition exits and destroys the old state before")
    lines.append(" * its pump builds the new one and calls its enter method, on")
    lines.append(" * the game's thread. (Until 2026-09-24 it set the slot's")
    lines.append(" * status byte itself, which skipped that teardown.)")
    lines.append(" * See cheats.h. Corrected 2026-08-31 -")
    lines.append(" * this text is EMITTED, so the stale version was rewritten into")
    lines.append(" * levels.h on every extraction. */")
    lines.append("static const unsigned kLsw2LevelRootAddr = 0x0210930Cu;")
    lines.append("static const unsigned kLsw2LevelObjOffset = 0x68u;")
    lines.append("static const unsigned kLsw2LevelIndexOffset = 0x479u;")
    lines.append("static const unsigned kLsw2LevelPermAddr = 0x020FF674u;")
    out.write_text("\n".join(lines), encoding="utf-8")

    counts = {}
    for _, _, _, source in final:
        counts[source] = counts.get(source, 0) + 1
    print("wrote %s" % out)
    for k in ("confirmed", "derived", "unknown"):
        print("   %-10s %d" % (k, counts.get(k, 0)))
    print("\nfirst rows:")
    for level_id, rec, label, source in final[:12]:
        print("   id %-3d rec %-3d %-28s %s" % (level_id, rec, label, source))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
