"""Read the PROJECTILE KIND table, so every hunter's shot is the right object.

WHY THIS EXISTS. The port fired one model - `blaster_particle/BlasterParticle.glb`
- for every character. The cartridge does not. Each action row's `+0x04` is a
projectile KIND, and the kind table gives each kind its own model. Boba Fett,
Dengar and Zuckuss all fire something visibly larger than a blaster bolt and the
port was drawing them a small one.

THE CHAIN, all disassembled:

    0x020F6BA8 + 0x24*row + 0x04     u8 PROJECTILE KIND
    0x020F4D0C + 0x0C*kind + 0x00    u16 WEAPON ID
    0x020EB7B8 + 8*weaponId + 0x04   u16 the descriptor set's TAG, which is the
                                     MODEL ASSET (the game loads this as
                                     0x020EB7BC + 8*id - see the note below)

The kind table's base and stride come from the reader itself:

    020187dc  mov   r1,#0xc            STRIDE 12
    020187e0  mul   r5,r7,r1           kind * 12
    020187ec  ldr   r1,[pc,#0x1f8]     pool 0x020189EC = 0x020F4D0C
    020187f4  ldrh  r2,[r1,r5]         field +0x00
    02018808  bl    0x200fe94

and a second reader takes a flags byte from the same rows:

    pool 0x02018A00 = 0x020F4D16 = 0x020F4D0C + 0x0A

FRAMING NOTE. 0x020EB7BC is not a table of its own: it is the TAG field of the
descriptor-set table at 0x020EB7B8, whose records are
{u32 array, u16 tag, u16 count}. The tag is the model asset the set animates,
which is why loading a halfword there gives a model. An earlier pass in this
project described 0x020EB7BC as a separate "weapon table" with fields
{model, flags, anims}; the model was right and the other two were the count and
the NEXT set's array.

THAT `+0x00` IS A WEAPON ID, NOT AN ASSET, and the proof is self-checking: kind
13 is Bossk's, its `+0x00` reads 550, and 0x020EB7BC + 8*550 gives asset 625
`BosskGrenade`. A grenade is what Bossk throws. The same walk turns kind 10 into
`BigBlasterPartic` and kind 0 into `BlasterParticle`, which is the model the port
already had for everyone.

WHERE THE TABLE ENDS. Field `+0x02` reads 630 - the no-weapon sentinel that
0x0209F404 stores as its constructor default and 0x020025F8 compares against -
for kinds 0 through 14, and garbage from 15 on. FIFTEEN KINDS, 0..14. Lando's
action row asks for kind 15, one past the end, which is consistent with
everything else about him: his dispatch byte `+0x03` is 0, his flags carry the
melee bit 0x40, and he swings an axe rather than firing anything.

WHAT THE TEN HUNTERS USE

    kind  0   BlasterParticle 429     4-LOM, Greedo, IG-88, Jango, Leia
    kind 10   BigBlasterPartic 352    Boba Fett, Dengar
    kind 11   BigBlasterPartic 352    Zuckuss   (same model, +0x07 = 2 not 1)
    kind 13   BosskGrenade 625        Bossk
    kind 15   -                       Lando, out of the table: melee

Zuckuss differs from Boba ONLY in byte `+0x07`, 2 against 1. He is the hunter
whose shot stuns, so `+0x07` is where that lives; the port already models the
stun from config/bounty_hunter_combat.json and this is where the ROM says it
comes from.

Usage:  python tools/export_projectile_kinds.py
"""

from __future__ import annotations

import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
CATALOG = ROOT / "out" / "raw" / "archive" / "catalog.json"
ACTION_ROWS = ROOT / "config" / "action_rows.json"
ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
EXPORT = ROOT / "out" / "exported" / "projectiles"
OUT = ROOT / "config" / "projectile_kinds.json"

BASE = 0x02000000
KIND_TABLE = 0x020F4D0C
KIND_STRIDE = 0x0C
DESCRIPTOR_SETS = 0x020EB7B8
WEAPON_TABLE = DESCRIPTOR_SETS + 4     # the TAG field, which the game loads
WEAPON_STRIDE = 8
NO_WEAPON = 630
KIND_COUNT = 15

LITERAL_CHECKS = {
    0x020189EC: KIND_TABLE,              # the +0x00 load at 0x020187EC
    0x02018A00: KIND_TABLE + 0x0A,       # the flags load
    0x0209F554: WEAPON_TABLE,            # the weapon-table load at 0x0209F53C
}

SKIP_NAMES = {"BMD0", "MDL0", "BCA0", "JNT0", "TEX0", "BTA0", "SRT0", "BTP0",
              "PAT0", "BTX0"}


def main() -> int:
    image = IMAGE.read_bytes()

    def u8(addr):
        return image[addr - BASE]

    def u16(addr):
        return struct.unpack_from("<H", image, addr - BASE)[0]

    def u32(addr):
        return struct.unpack_from("<I", image, addr - BASE)[0]

    for site, expected in LITERAL_CHECKS.items():
        got = u32(site)
        if got != expected:
            raise SystemExit("literal at 0x%08X reads 0x%08X, expected 0x%08X "
                             "- refusing to write" % (site, got, expected))

    catalog = {e["asset_id"]: e
               for e in json.loads(CATALOG.read_text(encoding="utf-8"))["entries"]}

    def name(asset):
        entry = catalog.get(asset)
        if entry is None:
            return ""
        names = [n for n in (entry.get("names") or []) if n not in SKIP_NAMES]
        return names[0] if names else ""

    # THE TABLE'S LENGTH IS MEASURED, NOT ASSUMED. Field +0x02 is the
    # no-weapon sentinel on every real row; the first row where it is not
    # marks the end.
    measured = 0
    while u16(KIND_TABLE + KIND_STRIDE * measured + 2) == NO_WEAPON:
        measured += 1
    if measured != KIND_COUNT:
        raise SystemExit("the kind table measures %d rows, expected %d"
                         % (measured, KIND_COUNT))

    kinds = []
    for kind in range(KIND_COUNT):
        row = KIND_TABLE + KIND_STRIDE * kind
        weapon_id = u16(row)
        model = u16(WEAPON_TABLE + WEAPON_STRIDE * weapon_id)
        kinds.append({
            "kind": kind,
            "row": "0x%08X" % row,
            "weapon_id": weapon_id,
            "model_asset": model,
            "model_name": name(model),
            "variant": u8(row + 7),
            "flags": "0x%02X" % u8(row + 0x0A),
        })

    by_kind = {k["kind"]: k for k in kinds}

    # Which hunter uses which kind, from the action rows.
    rows = json.loads(ACTION_ROWS.read_text(encoding="utf-8"))["rows"]
    characters = []
    for row in sorted(rows, key=lambda r: r["runtime_id"]):
        kind = row["projectile_kind"]
        entry = by_kind.get(kind)
        characters.append({
            "runtime_id": row["runtime_id"],
            "label": row["label"],
            "kind": kind,
            "in_table": entry is not None,
            "model_asset": entry["model_asset"] if entry else None,
            "model_name": entry["model_name"] if entry else "",
            "note": "" if entry else
                    "kind %d is past the end of the %d-row table - melee"
                    % (kind, KIND_COUNT),
        })

    # --- invariants -------------------------------------------------------
    if by_kind[13]["model_name"] != "BosskGrenade":
        raise SystemExit("kind 13 must resolve to BosskGrenade, got %r"
                         % by_kind[13]["model_name"])
    if by_kind[0]["model_name"] != "BlasterParticle":
        raise SystemExit("kind 0 must resolve to BlasterParticle, got %r"
                         % by_kind[0]["model_name"])
    if by_kind[10]["model_asset"] != by_kind[11]["model_asset"]:
        raise SystemExit("kinds 10 and 11 share a model in the ROM")
    if by_kind[10]["variant"] == by_kind[11]["variant"]:
        raise SystemExit("kinds 10 and 11 must differ at +0x07")
    by_label = {c["label"]: c for c in characters}
    if by_label["Lando Calrissian"]["in_table"]:
        raise SystemExit("Lando's kind must fall outside the table")

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "kind_table": "0x%08X" % KIND_TABLE,
            "kind_stride": KIND_STRIDE,
            "kind_count": KIND_COUNT,
            "count_measured_by": "field +0x02 holds the no-weapon sentinel 630 "
                                 "on every real row",
            "record": {
                "+0x00": "u16 weapon id -> 0x020EB7BC",
                "+0x02": "u16 sentinel 630",
                "+0x07": "u8 variant - 1 for kind 10, 2 for kind 11 (the stun)",
                "+0x0A": "u8 flags",
            },
            "reader": "0x020187DC .. 0x020187F4",
            "literal_checks": {"0x%08X" % k: "0x%08X" % v
                               for k, v in LITERAL_CHECKS.items()},
        },
        "note": __doc__.strip(),
        "kinds": kinds,
        "characters": characters,
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    # --- export the models the hunters actually use -----------------------
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"
    wanted = sorted({c["model_asset"] for c in characters
                     if c["model_asset"] is not None})
    exported = []
    for asset in wanted:
        matches = list(ENTRIES.glob("%04d.*" % asset))
        if len(matches) != 1:
            print("asset %d: %d archive entries, skipping" % (asset,
                                                              len(matches)))
            continue
        target = EXPORT / ("asset_%04d" % asset)
        target.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [str(apicula), "convert", str(matches[0]), "-o", str(target),
             "-f", "glb", "--more-textures", "--overwrite"],
            cwd=ROOT, capture_output=True, text=True)
        globs = sorted(target.glob("*.glb"))
        if result.returncode != 0 or not globs:
            print("asset %d: apicula failed - %s" % (asset,
                                                     result.stderr.strip()[:120]))
            continue
        exported.append((asset, globs[0].relative_to(ROOT)))

    print("%-4s %-8s %-8s %-24s %s" % ("kind", "weapId", "asset", "model",
                                       "variant"))
    for k in kinds:
        print("%-4d %-8d %-8d %-24s %d"
              % (k["kind"], k["weapon_id"], k["model_asset"], k["model_name"],
                 k["variant"]))
    print()
    for c in characters:
        print("  %-24s kind %-3d %s"
              % (c["label"], c["kind"], c["model_name"] or c["note"]))
    print()
    for asset, path in exported:
        print("  exported %d -> %s" % (asset, path))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
