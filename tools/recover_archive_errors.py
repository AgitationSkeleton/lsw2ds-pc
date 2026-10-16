#!/usr/bin/env python3
"""Retain raw entries that merely resemble type-6 wrappers by their first byte."""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import asdict
from pathlib import Path

import lsw2_archive as core


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("-o", "--output", type=Path, default=Path("out/raw/archive"))
    args = parser.parse_args()
    catalog_path = args.output / "catalog.json"
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    archive = core.read_rom_bin(args.rom)
    first = struct.unpack_from("<I", archive, 0)[0]
    offsets = struct.unpack_from(f"<{first // 4}I", archive, 0)
    recovered = []
    for warning in catalog.get("errors", []):
        asset_id = int(warning["asset_id"])
        payload = archive[offsets[asset_id] : offsets[asset_id + 1]]
        destination = args.output / "entries" / f"{asset_id:04d}.bin"
        destination.write_bytes(payload)
        record = core.ArchiveEntry(
            asset_id=asset_id,
            archive_offset=offsets[asset_id],
            packed_size=len(payload),
            packed_sha256=core.sha256(payload),
            wrapper=None,
            expanded_size=len(payload),
            expanded_sha256=core.sha256(payload),
            magic=payload[:4].hex(),
            extension="bin",
            names=core.printable_names(payload),
            output=destination.relative_to(args.output).as_posix(),
        )
        catalog["entries"].append(asdict(record))
        recovered.append({**warning, "resolution": "retained_packed_raw"})
    catalog["entries"].sort(key=lambda item: item["asset_id"])
    catalog["decode_warnings"] = recovered
    catalog["errors"] = []
    catalog["successful_entries"] = len(catalog["entries"])
    catalog_path.write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    print(f"Recovered {len(recovered)} raw entries; catalog now has {len(catalog['entries'])} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
