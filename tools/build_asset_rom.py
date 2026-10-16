"""Rebuild the LSW2 ROM with replacement assets - the modularity step.

WHAT THIS IS FOR. A Ship of Harkinian-style port lets people replace textures,
models and audio without touching game code. LSW2 keeps its content in a single
archive, `rom.bin`, inside NitroFS. This rebuilds that archive with any assets
you want swapped, splices it back into the cartridge image, and reports the new
SHA-1.

THE ARCHIVE FORMAT, confirmed against the ROM rather than assumed:

    rom.bin  offset 857,600 in the cartridge, 18,334,192 bytes
      [0]    u32 offset table, 5,549 entries for 5,548 assets
             - N+1 entries: each asset runs from table[i] to table[i+1]
      [...]  the payloads, starting at table[0] = 0x56B4 = 5549 * 4,
             i.e. immediately after the table

The N+1 shape is what makes this pipeline possible: because every asset's extent
is derived from the table rather than stored with the data, **a replacement does
not have to be the same size as the original.** The table is simply rewritten.

SIZE GROWTH IS HANDLED BY RELOCATION. If the rebuilt archive no longer fits
where it was, it is appended at the end of the ROM and the NitroFS FAT entry for
`rom.bin` is repointed. That is why this writes a whole new .nds rather than a
patch.

THE SHA-1 CONSEQUENCE, which is not optional and must be understood. The runner
registers our recompiled banks only for a cartridge whose SHA-1 matches
`NDS_TITLE_ROM_SHA1`. Changing any byte changes that hash, so a modded ROM needs
the runner reconfigured with the new value or **the game silently falls back to
the interpreter and crawls**.

The expensive part does not have to be redone. Banks are keyed to the ARM9 and
ARM7 *image* hashes, and this tool touches neither - only NitroFS data. So a
mod costs a CMake reconfigure and a relink, not a re-emit of 44,331 functions.

Usage:
    python tools/build_asset_rom.py                     # list what would change
    python tools/build_asset_rom.py --out modded.nds    # write it

Overrides live in `port/assets/override/` as `<asset_id>.bin`, holding the
PACKED bytes exactly as they sit in the archive (same wrapper as the original -
see the catalog's `wrapper` and `magic` fields). Nothing else is interpreted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CATALOG = ROOT / "out/raw/archive/catalog.json"
OVERRIDE_DIR = ROOT / "port/assets/override"

FAT_OFF_FIELD = 0x48          # header: NitroFS FAT offset
FAT_LEN_FIELD = 0x4C          # header: NitroFS FAT length
HEADER_CRC_FIELD = 0x15E      # CRC-16 over header bytes 0x000..0x15D


def crc16(data: bytes) -> int:
    """The CRC-16 the DS header uses (poly 0xA001, init 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def find_rom(root: pathlib.Path) -> pathlib.Path | None:
    roms = sorted(root.glob("rom/*.nds"))
    return roms[0] if roms else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the rebuilt cartridge here")
    ap.add_argument("--overrides", default=str(OVERRIDE_DIR),
                    help="directory of <asset_id>.bin replacements")
    args = ap.parse_args()

    if not CATALOG.exists():
        print("no catalog at %s - run the asset extraction first" % CATALOG)
        return 1
    rom_path = find_rom(ROOT)
    if rom_path is None:
        print("no ROM in rom/")
        return 1

    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    entries = catalog["entries"]
    rom = bytearray(rom_path.read_bytes())

    # Where the archive lives, from the extraction manifest rather than a
    # constant - the FAT is the authority and this must not drift from it.
    manifest = json.loads((ROOT / "out/raw/manifest.json").read_text(encoding="utf-8"))
    archive = next((f for f in manifest["files"] if f["path"] == "rom.bin"), None)
    if archive is None:
        print("manifest does not list rom.bin")
        return 1
    arc_off, arc_end = archive["offset"], archive["end"]
    original = bytes(rom[arc_off:arc_end])

    # Verify the format before trusting it. table[0] must equal (n+1)*4.
    count = len(entries)
    table_bytes = (count + 1) * 4
    first = struct.unpack_from("<I", original, 0)[0]
    if first != table_bytes:
        print("archive layout is not what was expected: table[0]=0x%X but "
              "(%d+1)*4=0x%X. Refusing to rebuild." % (first, count, table_bytes))
        return 1

    ovr_dir = pathlib.Path(args.overrides)
    overrides = {}
    if ovr_dir.exists():
        for p in sorted(ovr_dir.glob("*.bin")):
            try:
                overrides[int(p.stem)] = p.read_bytes()
            except ValueError:
                print("skipping %s - name is not an asset id" % p.name)

    if not overrides:
        print("no overrides in %s" % ovr_dir)
        print("archive verified: %d assets, table[0]=0x%X, %d bytes"
              % (count, first, len(original)))
        print("place <asset_id>.bin files there and re-run with --out")
        return 0

    # Rebuild: table first, then payloads in asset order.
    payloads = []
    delta = 0
    for e in entries:
        aid = e["asset_id"]
        if aid in overrides:
            data = overrides[aid]
            delta += len(data) - e["packed_size"]
        else:
            o = e["archive_offset"]
            data = original[o:o + e["packed_size"]]
        payloads.append(data)

    table = [table_bytes]
    for data in payloads:
        table.append(table[-1] + len(data))
    rebuilt = bytearray(struct.pack("<%dI" % len(table), *table))
    for data in payloads:
        rebuilt += data

    print("overrides applied : %d" % len(overrides))
    for aid in sorted(overrides):
        e = entries[aid]
        print("   asset %-5d %-6s %8d -> %8d bytes"
              % (aid, e.get("magic") or "?", e["packed_size"], len(overrides[aid])))
    print("archive size      : %d -> %d (%+d)"
          % (len(original), len(rebuilt), len(rebuilt) - len(original)))

    if args.out is None:
        print("\ndry run - pass --out <file.nds> to write it")
        return 0

    # Place the rebuilt archive. In place when it fits, appended otherwise.
    if len(rebuilt) <= len(original):
        rom[arc_off:arc_off + len(rebuilt)] = rebuilt
        # Leave the tail as it was; the FAT end marker is what bounds the file.
        new_off, new_end = arc_off, arc_off + len(rebuilt)
        placement = "in place"
    else:
        pad = (-len(rom)) & 0x1FF
        rom += b"\xFF" * pad
        new_off = len(rom)
        rom += rebuilt
        new_end = len(rom)
        placement = "relocated to the end of the image (it grew)"

        # THE CARTRIDGE ADDRESS MASK REQUIRES A POWER-OF-TWO IMAGE. The runner
        # serves reads with
        #
        #     address &= (uint32_t)(g_card_rom.size() - 1u);
        #
        # which is only a correct wrap for a power-of-two size. The stock ROM is
        # exactly 32 MB so this never comes up - but a grown image lands on
        # something like 0x0317D1F0, and every read would then be masked against
        # a value with holes in it, silently returning the wrong bytes from
        # somewhere else in the cartridge.
        #
        # Pad up to the next power of two, and tell the header about it: field
        # 0x14 is the device capacity as a shift, size = 128KB << capacity.
        target = 1
        while target < len(rom):
            target <<= 1
        if target != len(rom):
            rom += b"\xFF" * (target - len(rom))
        capacity = 0
        while (128 * 1024) << capacity < target:
            capacity += 1
        rom[0x14] = capacity
        placement += ("; padded to 0x%X (a power of two - the cartridge "
                      "address mask requires it) and header capacity set to %d"
                      % (target, capacity))

    # Repoint the NitroFS FAT entry. rom.bin is file_id 0, so it is the first
    # 8-byte record: {u32 start, u32 end}.
    fat_off = struct.unpack_from("<I", rom, FAT_OFF_FIELD)[0]
    fid = archive.get("file_id", 0)
    struct.pack_into("<II", rom, fat_off + fid * 8, new_off, new_end)

    # The header CRC covers 0x000..0x15D and would otherwise be stale.
    struct.pack_into("<H", rom, HEADER_CRC_FIELD, crc16(bytes(rom[:0x15E])))

    out = pathlib.Path(args.out)
    out.write_bytes(bytes(rom))
    sha1 = hashlib.sha1(bytes(rom)).hexdigest()

    print("\narchive placement : %s" % placement)
    print("FAT entry %d      : 0x%08X..0x%08X" % (fid, new_off, new_end))
    print("wrote             : %s (%d bytes)" % (out, len(rom)))
    print("SHA-1             : %s" % sha1)
    print()
    print("REBUILD THE RUNNER FOR THIS HASH or the banks will not register and")
    print("the game will run entirely in the interpreter:")
    print("    NDS_TITLE_ROM_SHA1=%s" % sha1)
    print("Banks do NOT need re-emitting - they are keyed to the ARM9/ARM7")
    print("image hashes, and this only touched NitroFS data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
