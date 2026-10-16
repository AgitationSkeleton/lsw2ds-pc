"""Read the material block out of an NSBMD, with the walk verified end to end.

WHY THIS EXISTS. The port has been taking material properties from whatever the
GLB converter chose to emit, which is a second-hand reading. Blend mode, alpha,
cull, diffuse and emission are all stated by the model itself, and getting at
them needed the material block located - which took two passes and one wrong
conclusion that had to be retracted.

THE WALK, for CantinaArenaLobb (asset 955):

    0x1C    model dict: 1 entry; model 0 sits at offset 0x30 from the MDL0
            section, and MDL0 is at 0x14, so the MODEL BASE is 0x44.
    0x44    model header. +0x00 size, +0x04 renderCmds, +0x08 materialsOffset,
            +0x0C shapes, +0x10 envelopes. materialsOffset = 0x03D4, so the
            MATERIALS BLOCK is 0x418.
    0x418   materials block: two u16 sub-dict offsets, then the material dict
            at +4.
    0x41C   the dict: u8 dummy, u8 numEntries, u16 sectionSize, then an unknown
            block, then u16 entrySize / u16 namesOffset, then numEntries u32
            entries, then numEntries 16-byte names.

Dict entry offsets are relative to the MATERIALS BLOCK, not to the file.

THE MATERIAL RECORD, offsets within each material:

    +0x04  u32 diffAmb    diffuse RGB555 in bits 0..14, ambient in 16..30
    +0x08  u32 specEmi    specular in bits 0..14, emission in 16..30
    +0x0C  u32 polyAttr   ALPHA in bits 16..20 (0..31), CULL in bits 6..7
    +0x14  u32 texImageParam    0 when the material carries no texture

THE PARSE IS CROSS-CHECKED RATHER THAN ASSUMED. polyAttr bits 6-7 are the cull
mode, and the values this reads - 0x80 for cull-back, 0xC0 for cull-disabled -
match what apicula independently produced for the same materials in the exported
GLB. Two readers agreeing on a field neither was told about is the check.

WHAT IT ALREADY SETTLED. Cantina_Hologram reads polyAttr 0x001F0080: alpha
31/31, FULLY OPAQUE, texImageParam 0. A previous pass had forced that surface
translucent on the theory that the converter dropped its vertex alpha; the model
says otherwise and that change was retracted.

Usage:
    python tools/export_nsbmd_materials.py                # the lobby and both arenas
    python tools/export_nsbmd_materials.py 322 327 955    # any assets
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
OUT = ROOT / "config" / "nsbmd_materials.json"

MDL0_SECTION = 0x14
NAME_WIDTH = 16

CULL_NAMES = {0: "none", 1: "front", 2: "back", 3: "disabled"}


def rgb555(value: int) -> list[int]:
    return [(value & 31) * 8, ((value >> 5) & 31) * 8, ((value >> 10) & 31) * 8]


def read_model(data: bytes) -> dict:
    def u16(o):
        return struct.unpack_from("<H", data, o)[0]

    def u32(o):
        return struct.unpack_from("<I", data, o)[0]

    if data[:4] != b"BMD0":
        raise ValueError("not a BMD0")

    # The model dictionary sits straight after the MDL0 header.
    dict_at = MDL0_SECTION + 8
    model_count = data[dict_at + 1]
    if model_count < 1:
        raise ValueError("no models")
    # The dict header is: dummy, numEntries, sectionSize, unknownBlockSize,
    # unknownBlockOffset, then an unknown block of 4 + numEntries*4 bytes, then
    # u16 entrySize / u16 namesOffset, then the entries, then the names.
    #
    # Checked by hand against BOTH dicts in asset 955: the model dict at 0x1C
    # with one entry puts entrySize/namesOffset at 0x2C and the entry at 0x30,
    # and the material dict at 0x41C with 27 entries puts them at 0x494 and
    # 0x498. dict + 12 + count*4 lands on both.
    model_sizes_at = dict_at + 12 + model_count * 4
    entries_at = model_sizes_at + 4
    model_offset = u32(entries_at)
    model_base = MDL0_SECTION + model_offset

    materials_base = model_base + u32(model_base + 0x08)

    # The material dict lives 4 bytes into the materials block.
    mdict = materials_base + 4
    count = data[mdict + 1]
    # u16 entrySize / u16 namesOffset follow the unknown block.
    sizes_at = mdict + 12 + count * 4
    entry_size = u16(sizes_at)
    names_offset = u16(sizes_at + 2)
    entry_at = sizes_at + 4
    names_at = sizes_at + names_offset

    if entry_size != 4:
        raise ValueError("unexpected material entry size %d" % entry_size)

    materials = []
    for i in range(count):
        name = data[names_at + i * NAME_WIDTH:
                    names_at + i * NAME_WIDTH + NAME_WIDTH]
        name = name.split(b"\0")[0].decode("latin1")
        offset = u32(entry_at + i * 4)
        at = materials_base + offset
        diff_amb = u32(at + 0x04)
        spec_emi = u32(at + 0x08)
        poly = u32(at + 0x0C)
        tex = u32(at + 0x14)
        materials.append({
            "index": i,
            "name": name,
            "address": "0x%04X" % at,
            "diffuse": rgb555(diff_amb & 0x7FFF),
            "ambient": rgb555((diff_amb >> 16) & 0x7FFF),
            "specular": rgb555(spec_emi & 0x7FFF),
            "emission": rgb555((spec_emi >> 16) & 0x7FFF),
            "poly_attr": "0x%08X" % poly,
            "alpha": (poly >> 16) & 0x1F,
            "cull": CULL_NAMES[(poly >> 6) & 3],
            "textured": tex != 0,
            "tex_image_param": "0x%08X" % tex,
        })

    return {
        "model_base": "0x%04X" % model_base,
        "materials_base": "0x%04X" % materials_base,
        "count": count,
        "materials": materials,
    }


def main() -> int:
    # THE BASE SET IS ALWAYS INCLUDED, not merely the default.
    #
    # Making it a default was not enough: running the tool to look at some other
    # model - `export_nsbmd_materials.py 2524 2522 2523 818` while investigating
    # the grapple - replaced the file with just those, dropped the lobby and both
    # arenas, and broke test_nsbmd_materials.gd for the SECOND time.
    #
    # config/nsbmd_materials.json is a config the port and the tests read, so an
    # exploratory run must ADD to it rather than replace it. Extras are appended
    # to the base set and the output keeps whatever it already held for models
    # not asked for this run.
    BASE_ASSETS = [955, 322, 327]
    extras = [int(a) for a in sys.argv[1:]]
    assets = BASE_ASSETS + [a for a in extras if a not in BASE_ASSETS]
    models = {}
    for asset in assets:
        path = ENTRIES / ("%04d.nsbmd" % asset)
        if not path.exists():
            print("asset %d: no .nsbmd at %s" % (asset, path.name))
            continue
        try:
            model = read_model(path.read_bytes())
        except Exception as error:  # noqa: BLE001
            print("asset %d: %s" % (asset, error))
            continue
        models[str(asset)] = model
        print("asset %-5d model_base %s  materials %s  %d materials"
              % (asset, model["model_base"], model["materials_base"],
                 model["count"]))
        opaque = sum(1 for m in model["materials"] if m["alpha"] == 31)
        untextured = [m["name"] for m in model["materials"] if not m["textured"]]
        print("   alpha 31/31: %d of %d      untextured: %d  %s"
              % (opaque, model["count"], len(untextured), untextured[:6]))
        for m in model["materials"]:
            if m["alpha"] != 31 or m["cull"] != "back":
                print("   %-18s alpha %2d/31  cull %-9s tex %s  %s"
                      % (m["name"], m["alpha"], m["cull"],
                         "yes" if m["textured"] else "NO ", m["poly_attr"]))

    if not models:
        raise SystemExit("no models read")

    # --- the invariants that pin the walk --------------------------------
    lobby = models.get("955")
    if lobby:
        if lobby["materials_base"] != "0x0418":
            raise SystemExit("the lobby materials block is 0x418, got %s"
                             % lobby["materials_base"])
        if lobby["count"] != 27:
            raise SystemExit("the lobby has 27 materials, got %d" % lobby["count"])
        by_name = {m["name"]: m for m in lobby["materials"]}
        body = by_name.get("Cantina_Hologram")
        if not body:
            raise SystemExit("Cantina_Hologram missing")
        if body["poly_attr"] != "0x001F0080" or body["alpha"] != 31:
            raise SystemExit("Cantina_Hologram must read 0x001F0080, alpha 31 - "
                             "got %s, %d" % (body["poly_attr"], body["alpha"]))
        if body["textured"]:
            raise SystemExit("Cantina_Hologram carries no texture")
        # The cull cross-check: the converter produced disabled for Glow and
        # back for the other two, and this must agree without being told.
        if by_name["Hologram_Glow"]["cull"] != "disabled":
            raise SystemExit("Hologram_Glow is cull-disabled in the export")
        if by_name["Hologram_ScanLin"]["cull"] != "back":
            raise SystemExit("Hologram_ScanLin is cull-back in the export")

    # Keep anything a previous run recorded for models not in this one.
    if OUT.exists():
        try:
            previous = json.loads(OUT.read_text(encoding="utf-8"))
            for key, value in (previous.get("models") or {}).items():
                models.setdefault(key, value)
        except Exception:  # noqa: BLE001
            pass

    document = {
        "schema": 1,
        "note": __doc__.strip(),
        "record": {
            "+0x04": "u32 diffAmb - diffuse RGB555 bits 0..14, ambient 16..30",
            "+0x08": "u32 specEmi - specular bits 0..14, emission 16..30",
            "+0x0C": "u32 polyAttr - ALPHA bits 16..20 (0..31), CULL bits 6..7",
            "+0x14": "u32 texImageParam - 0 when untextured",
        },
        "models": models,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")
    print("\nwrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
