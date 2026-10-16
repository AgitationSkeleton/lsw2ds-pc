#!/usr/bin/env python3
"""Catalog and extract LEGO Star Wars II DS's offset-table asset archive.

The game's ``rom.bin`` is a flat table of 32-bit offsets followed by asset data.
Most models and texture payloads use a proprietary type-6 wrapper. Rather than
embedding that routine, this tool recovers the supplied ROM's ARM9 ITCM autoload and
executes its decoder in a bounded Unicorn ARM sandbox. The original ROM is thus
required for every extraction.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


TOOL_DIR = Path(__file__).resolve().parent
VENDOR_PYTHON = TOOL_DIR / "vendor" / "python"
if VENDOR_PYTHON.is_dir():
    sys.path.insert(0, str(VENDOR_PYTHON))

try:
    import ndspy.rom
    from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, Uc, UcError
    from unicorn.arm_const import (
        UC_ARM_REG_LR,
        UC_ARM_REG_R0,
        UC_ARM_REG_R1,
        UC_ARM_REG_R2,
        UC_ARM_REG_SP,
    )
except ImportError as exc:  # pragma: no cover - exercised by a clean checkout
    raise SystemExit(
        "missing local dependencies; run tools/install_vendor.ps1 first "
        f"({exc})"
    ) from exc

from nds_extract import parse_fat, parse_fnt, parse_header


ITCM_ADDRESS = 0x01FF8000
SOURCE_ADDRESS = 0x03000000
OUTPUT_ADDRESS = 0x04000000
STACK_ADDRESS = 0x05000000
RETURN_ADDRESS = 0x06000000
MAX_EXPANDED_SIZE = 64 * 1024 * 1024

NITRO_EXTENSIONS = {
    b"BMD0": "nsbmd",
    b"BTX0": "nsbtx",
    b"BCA0": "nsbca",
    b"BVA0": "nsbva",
    b"BTA0": "nsbta",
    b"BTP0": "nsbtp",
    b"BMA0": "nsbma",
}


def align_page(size: int) -> int:
    return max(0x1000, (size + 0xFFF) & ~0xFFF)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def printable_names(data: bytes, limit: int = 24) -> list[str]:
    result: list[str] = []
    for match in re.finditer(rb"[A-Za-z][A-Za-z0-9_./-]{3,}", data):
        value = match.group().decode("ascii", "replace")
        if value not in result:
            result.append(value)
            if len(result) == limit:
                break
    return result


def read_rom_bin(rom_path: Path) -> bytes:
    rom = rom_path.read_bytes()
    header = parse_header(rom)
    fat = parse_fat(rom, header)
    names = parse_fnt(rom, header, len(fat))
    for file_id, name in names.items():
        if name.casefold() == "rom.bin":
            start, end = fat[file_id]
            return rom[start:end]
    raise ValueError("NitroFS does not contain rom.bin")


def load_itcm(rom_path: Path) -> bytes:
    rom = ndspy.rom.NintendoDSRom.fromFile(str(rom_path))
    for section in rom.loadArm9().sections:
        if section.ramAddress == ITCM_ADDRESS:
            return bytes(section.data)
    raise ValueError(f"ARM9 has no ITCM autoload at 0x{ITCM_ADDRESS:08X}")


class Type6Decoder:
    """Execute the ROM-supplied ITCM decoder with isolated scratch memory."""

    def __init__(self, itcm: bytes):
        self.itcm = itcm

    def decode(self, wrapped: bytes) -> bytes:
        if len(wrapped) < 16:
            raise ValueError("type-6 stream is shorter than its headers")
        header = struct.unpack_from("<I", wrapped, 0)[0]
        codec = (wrapped[0] >> 4) & 7
        expanded_size = header >> 8
        if codec != 6:
            raise ValueError(f"expected codec 6, got {codec}")
        if not 0 < expanded_size <= MAX_EXPANDED_SIZE:
            raise ValueError(f"implausible expanded size: {expanded_size}")

        payload = wrapped[4:]
        emu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        emu.mem_map(ITCM_ADDRESS, align_page(len(self.itcm)))
        emu.mem_map(SOURCE_ADDRESS, align_page(len(payload)))
        emu.mem_map(OUTPUT_ADDRESS, align_page(expanded_size))
        emu.mem_map(STACK_ADDRESS, 0x10000)
        emu.mem_map(RETURN_ADDRESS, 0x1000)
        emu.mem_write(ITCM_ADDRESS, self.itcm)
        emu.mem_write(SOURCE_ADDRESS, payload)
        emu.reg_write(UC_ARM_REG_R0, SOURCE_ADDRESS)
        emu.reg_write(UC_ARM_REG_R1, OUTPUT_ADDRESS)
        emu.reg_write(UC_ARM_REG_R2, STACK_ADDRESS)
        emu.reg_write(UC_ARM_REG_SP, STACK_ADDRESS + 0xFFF0)
        emu.reg_write(UC_ARM_REG_LR, RETURN_ADDRESS)

        returned = False

        def stop_on_return(instance: Uc, address: int, _size: int, _data: object) -> None:
            nonlocal returned
            if address == RETURN_ADDRESS:
                returned = True
                instance.emu_stop()

        emu.hook_add(UC_HOOK_CODE, stop_on_return)
        try:
            emu.emu_start(ITCM_ADDRESS, RETURN_ADDRESS + 4, count=100_000_000)
        except UcError as exc:
            raise ValueError(f"ITCM decoder failed: {exc}") from exc
        if not returned:
            raise ValueError("ITCM decoder hit its instruction limit")
        return bytes(emu.mem_read(OUTPUT_ADDRESS, expanded_size))


@dataclass
class ArchiveEntry:
    asset_id: int
    archive_offset: int
    packed_size: int
    packed_sha256: str
    wrapper: str | None
    expanded_size: int
    expanded_sha256: str
    magic: str
    extension: str
    names: list[str]
    output: str


def extract_archive(rom_path: Path, output: Path) -> dict:
    archive = read_rom_bin(rom_path)
    if len(archive) < 8:
        raise ValueError("rom.bin is too small")
    first_offset = struct.unpack_from("<I", archive, 0)[0]
    if first_offset % 4 or first_offset < 8 or first_offset > len(archive):
        raise ValueError(f"invalid first asset offset 0x{first_offset:X}")
    offsets = list(struct.unpack_from(f"<{first_offset // 4}I", archive, 0))
    if offsets[-1] != len(archive):
        raise ValueError("final offset is not the rom.bin end sentinel")
    if any(a > b for a, b in zip(offsets, offsets[1:])):
        raise ValueError("asset offsets are not monotonic")

    decoder = Type6Decoder(load_itcm(rom_path))
    entries_dir = output / "entries"
    entries_dir.mkdir(parents=True, exist_ok=True)
    records: list[ArchiveEntry] = []
    errors: list[dict] = []

    # The final offset is a sentinel; asset IDs are indices between offset pairs.
    for asset_id, (start, end) in enumerate(zip(offsets, offsets[1:])):
        packed = archive[start:end]
        expanded = packed
        wrapper: str | None = None
        if len(packed) >= 16 and ((packed[0] >> 4) & 7) == 6:
            wrapper = "type6_itcm"
            try:
                expanded = decoder.decode(packed)
            except ValueError as exc:
                errors.append({"asset_id": asset_id, "offset": start, "error": str(exc)})
                continue

        magic_bytes = expanded[:4]
        extension = NITRO_EXTENSIONS.get(magic_bytes, "bin")
        magic = magic_bytes.decode("ascii", "replace") if extension != "bin" else magic_bytes.hex()
        filename = f"{asset_id:04d}.{extension}"
        destination = entries_dir / filename
        destination.write_bytes(expanded)
        records.append(
            ArchiveEntry(
                asset_id=asset_id,
                archive_offset=start,
                packed_size=len(packed),
                packed_sha256=sha256(packed),
                wrapper=wrapper,
                expanded_size=len(expanded),
                expanded_sha256=sha256(expanded),
                magic=magic,
                extension=extension,
                names=printable_names(expanded),
                output=destination.relative_to(output).as_posix(),
            )
        )

    manifest = {
        "source_rom": rom_path.name,
        "source_rom_sha256": sha256(rom_path.read_bytes()),
        "archive_sha256": sha256(archive),
        "offset_count": len(offsets),
        "asset_count": len(offsets) - 1,
        "successful_entries": len(records),
        "errors": errors,
        "entries": [asdict(record) for record in records],
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / "catalog.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="path to your own LSW2 DS ROM")
    parser.add_argument("-o", "--output", type=Path, default=Path("out/raw/archive"))
    args = parser.parse_args()
    try:
        manifest = extract_archive(args.rom, args.output)
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"Extracted {manifest['successful_entries']}/{manifest['asset_count']} assets "
        f"({len(manifest['errors'])} errors) to {args.output.resolve()}"
    )
    return 0 if not manifest["errors"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
