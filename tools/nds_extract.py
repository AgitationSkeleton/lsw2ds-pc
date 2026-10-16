#!/usr/bin/env python3
"""Dependency-free Nintendo DS ROM header/NitroFS extractor.

This tool intentionally performs only documented cartridge-container parsing. It
does not decrypt, patch, or redistribute game content. Extracted binaries and
NitroFS files belong under the repository's ignored ``out/raw`` directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath


HEADER_SIZE = 0x200


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


@dataclass(frozen=True)
class Segment:
    name: str
    offset: int
    size: int
    ram_address: int | None = None
    entry_address: int | None = None


@dataclass(frozen=True)
class NitroFile:
    file_id: int
    path: str
    offset: int
    end: int
    size: int
    sha256: str


def read_region(rom: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(rom):
        raise ValueError(
            f"{label} region 0x{offset:X}+0x{size:X} exceeds ROM size 0x{len(rom):X}"
        )
    return rom[offset : offset + size]


def parse_header(rom: bytes) -> dict:
    if len(rom) < HEADER_SIZE:
        raise ValueError("file is too small to contain a Nintendo DS header")
    h = rom[:HEADER_SIZE]
    segments = [
        Segment("arm9", u32(h, 0x20), u32(h, 0x2C), u32(h, 0x28), u32(h, 0x24)),
        Segment("arm7", u32(h, 0x30), u32(h, 0x3C), u32(h, 0x38), u32(h, 0x34)),
        Segment("fnt", u32(h, 0x40), u32(h, 0x44)),
        Segment("fat", u32(h, 0x48), u32(h, 0x4C)),
        Segment("arm9_overlay_table", u32(h, 0x50), u32(h, 0x54)),
        Segment("arm7_overlay_table", u32(h, 0x58), u32(h, 0x5C)),
    ]
    return {
        "title": h[0:12].rstrip(b"\0").decode("ascii", "replace"),
        "game_code": h[0x0C:0x10].decode("ascii", "replace"),
        "maker_code": h[0x10:0x12].decode("ascii", "replace"),
        "unit_code": h[0x12],
        "device_capacity_exponent": h[0x14],
        "rom_size": len(rom),
        "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "banner_offset": u32(h, 0x68),
        "header_crc16_stored": f"0x{u16(h, 0x15E):04X}",
        "segments": [asdict(segment) for segment in segments],
    }


def parse_fat(rom: bytes, header: dict) -> list[tuple[int, int]]:
    segment = next(s for s in header["segments"] if s["name"] == "fat")
    fat = read_region(rom, segment["offset"], segment["size"], "FAT")
    if len(fat) % 8:
        raise ValueError(f"FAT size 0x{len(fat):X} is not divisible by 8")
    entries = []
    for index in range(0, len(fat), 8):
        start, end = struct.unpack_from("<II", fat, index)
        if start > end or end > len(rom):
            raise ValueError(f"invalid FAT entry {index // 8}: 0x{start:X}..0x{end:X}")
        entries.append((start, end))
    return entries


def parse_fnt(rom: bytes, header: dict, file_count: int) -> dict[int, str]:
    segment = next(s for s in header["segments"] if s["name"] == "fnt")
    fnt = read_region(rom, segment["offset"], segment["size"], "FNT")
    if len(fnt) < 8:
        raise ValueError("FNT is too small")

    root_subtable, root_first_file, directory_count = struct.unpack_from("<IHH", fnt, 0)
    if directory_count == 0 or directory_count * 8 > len(fnt):
        raise ValueError(f"invalid FNT directory count: {directory_count}")

    dirs: dict[int, tuple[int, int, int]] = {}
    for index in range(directory_count):
        subtable, first_file, parent = struct.unpack_from("<IHH", fnt, index * 8)
        dirs[0xF000 + index] = (subtable, first_file, parent)

    names: dict[int, str] = {}
    visiting: set[int] = set()

    def walk(directory_id: int, prefix: PurePosixPath) -> None:
        if directory_id in visiting:
            raise ValueError(f"FNT directory cycle at 0x{directory_id:04X}")
        if directory_id not in dirs:
            raise ValueError(f"FNT references unknown directory 0x{directory_id:04X}")
        visiting.add(directory_id)
        cursor, file_id, _parent = dirs[directory_id]
        if cursor >= len(fnt):
            raise ValueError(f"FNT subtable offset 0x{cursor:X} is out of bounds")
        while True:
            length_and_type = fnt[cursor]
            cursor += 1
            if length_and_type == 0:
                break
            is_directory = bool(length_and_type & 0x80)
            name_length = length_and_type & 0x7F
            if name_length == 0 or cursor + name_length > len(fnt):
                raise ValueError("malformed FNT name entry")
            raw_name = fnt[cursor : cursor + name_length]
            cursor += name_length
            name = raw_name.decode("ascii", "replace").replace("\\", "_").replace("/", "_")
            if name in {"", ".", ".."}:
                name = f"unnamed_{cursor:X}"
            path = prefix / name
            if is_directory:
                if cursor + 2 > len(fnt):
                    raise ValueError("truncated FNT child directory entry")
                child_id = u16(fnt, cursor)
                cursor += 2
                walk(child_id, path)
            else:
                if file_id >= file_count:
                    raise ValueError(f"FNT file id {file_id} exceeds FAT count {file_count}")
                names[file_id] = path.as_posix()
                file_id += 1
        visiting.remove(directory_id)

    # The first two fields are useful sanity checks and make corrupt tables easier to diagnose.
    if root_subtable != dirs[0xF000][0] or root_first_file != dirs[0xF000][1]:
        raise ValueError("inconsistent root FNT entry")
    walk(0xF000, PurePosixPath())
    return names


def safe_output_path(root: Path, relative: str) -> Path:
    parts = PurePosixPath(relative).parts
    if not parts or any(part in {"", ".", ".."} for part in parts):
        raise ValueError(f"unsafe output path: {relative!r}")
    candidate = root.joinpath(*parts)
    if root.resolve() not in candidate.resolve().parents:
        raise ValueError(f"output path escaped root: {relative!r}")
    return candidate


def extract(rom_path: Path, output: Path, list_only: bool) -> dict:
    rom = rom_path.read_bytes()
    header = parse_header(rom)
    fat = parse_fat(rom, header)
    names = parse_fnt(rom, header, len(fat))
    files: list[NitroFile] = []

    for file_id, (start, end) in enumerate(fat):
        relative = names.get(file_id, f"__unnamed/file_{file_id:05d}.bin")
        payload = rom[start:end]
        files.append(
            NitroFile(file_id, relative, start, end, len(payload), hashlib.sha256(payload).hexdigest())
        )
        if not list_only:
            destination = safe_output_path(output / "nitrofs", relative)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(payload)

    manifest = {"header": header, "nitrofs_file_count": len(files), "files": [asdict(f) for f in files]}
    if not list_only:
        output.mkdir(parents=True, exist_ok=True)
        (output / "header.bin").write_bytes(rom[:HEADER_SIZE])
        for segment in header["segments"]:
            if segment["size"]:
                data = read_region(rom, segment["offset"], segment["size"], segment["name"])
                (output / f"{segment['name']}.bin").write_bytes(data)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="path to a user-supplied Nintendo DS ROM")
    parser.add_argument("-o", "--output", type=Path, default=Path("out/raw"))
    parser.add_argument("--list", action="store_true", help="print manifest JSON without extracting")
    args = parser.parse_args()
    try:
        manifest = extract(args.rom, args.output, args.list)
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.list:
        print(json.dumps(manifest, indent=2))
    else:
        h = manifest["header"]
        print(f"{h['title']} ({h['game_code']}), {manifest['nitrofs_file_count']} NitroFS files")
        print(f"ROM SHA-256: {h['rom_sha256']}")
        print(f"Extracted to: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
