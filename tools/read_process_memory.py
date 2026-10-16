#!/usr/bin/env python3
"""Read a running melonDS's emulated memory straight out of the process.

The third way to ask "what is this screen actually using", after the offline
savestate reader and the GDB stub:

  tools/read_savestate.py     - offline, needs a state saved first
  tools/gdb_read.py           - live, but halts the CPU and wedges easily
  this                        - live, read-only, disturbs nothing

The emulator keeps the DS's main RAM and VRAM as ordinary allocations in its
own address space, so ReadProcessMemory reaches them with the game still
running at full speed. Nothing is written and the emulator is never paused, so
this is safe to run during a live multiplayer match.

Main RAM is located exactly as the savestate reader does it: match a signature
lifted from the extracted ARM9 image, then confirm with a second, distant
signature so a chance hit cannot mislocate it.

Sprite residency is then decided by searching every committed region for each
sprite's pixel bytes - VRAM included, which is where the graphics actually land
and which is not inside the 4 MB main RAM window.

Run:
    python tools/read_process_memory.py --list
    python tools/read_process_memory.py --sprites
    python tools/read_process_memory.py --dump-ram out/work/live_ram.bin
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import read_savestate  # noqa: E402  - shares the sprite table and ARM9 probes

ROOT = Path(__file__).resolve().parents[1]
ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
MEM_IMAGE = 0x1000000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = {0x02, 0x04, 0x20, 0x40, 0x80}  # R, RW, XR, XRW, XWC

MAIN_RAM_SIZE = 4 * 1024 * 1024
SIGNATURE_BYTES = 64
# Regions smaller than this cannot be main RAM and rarely hold VRAM banks.
MIN_REGION = 16 * 1024


class MemoryBasicInformation(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("__alignment1", wintypes.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__alignment2", wintypes.DWORD),
    ]


class Process:
    def __init__(self, pid: int):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32 = kernel32
        self.handle = kernel32.OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid
        )
        if not self.handle:
            raise SystemExit(
                f"could not open pid {pid}: error {ctypes.get_last_error()}"
            )

    def close(self) -> None:
        self.kernel32.CloseHandle(self.handle)

    def regions(self) -> list[tuple[int, int]]:
        """Committed, readable, non-image regions, as (base, size)."""
        found, address = [], 0
        info = MemoryBasicInformation()
        while address < 0x7FFF_FFFF_0000:
            got = self.kernel32.VirtualQueryEx(
                self.handle, ctypes.c_void_p(address), ctypes.byref(info),
                ctypes.sizeof(info),
            )
            if not got:
                break
            size = info.RegionSize
            if (info.State == MEM_COMMIT
                    and info.Type != MEM_IMAGE
                    and info.Protect & 0xFF in READABLE
                    and not info.Protect & PAGE_GUARD
                    and size >= MIN_REGION):
                found.append((info.BaseAddress or address, size))
            address += size if size else 0x1000
        return found

    def read(self, address: int, length: int) -> bytes | None:
        buffer = ctypes.create_string_buffer(length)
        read = ctypes.c_size_t(0)
        ok = self.kernel32.ReadProcessMemory(
            self.handle, ctypes.c_void_p(address), buffer, length,
            ctypes.byref(read),
        )
        if not ok or read.value == 0:
            return None
        return buffer.raw[: read.value]


def find_melonds_pid() -> int:
    import subprocess
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process melonDS -ErrorAction SilentlyContinue | "
         "Select-Object -First 1).Id"],
        capture_output=True, text=True,
    ).stdout.strip()
    if not out.isdigit():
        raise SystemExit("melonDS is not running; pass --pid explicitly")
    return int(out)


def locate_main_ram(process: Process, regions, arm9: bytes):
    """Return (region_base, offset_in_region) of the DS's main RAM."""
    for probe in read_savestate.PROBE_OFFSETS:
        signature = arm9[probe : probe + SIGNATURE_BYTES]
        if len(signature) < SIGNATURE_BYTES or signature.count(0) > read_savestate.FLAT_LIMIT:
            continue
        confirm = arm9[read_savestate.CONFIRM_OFFSET :
                       read_savestate.CONFIRM_OFFSET + SIGNATURE_BYTES]
        for base, size in regions:
            if size < MAIN_RAM_SIZE:
                continue
            data = process.read(base, size)
            if not data:
                continue
            position = data.find(signature)
            while position >= 0:
                start = position - probe
                if start >= 0:
                    at = start + read_savestate.CONFIRM_OFFSET
                    if data[at : at + SIGNATURE_BYTES] == confirm:
                        return base, start
                position = data.find(signature, position + 1)
    return None, None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="melonDS pid (auto-detected)")
    parser.add_argument("--list", action="store_true", help="list candidate regions")
    parser.add_argument("--sprites", action="store_true", help="report resident sprites")
    parser.add_argument("--dump-ram", type=Path, help="write the 4 MB main RAM")
    parser.add_argument("--output", type=Path, help="write the report as JSON")
    arguments = parser.parse_args()
    if not ARM9.exists():
        raise SystemExit("run the ROM extraction step first")

    pid = arguments.pid or find_melonds_pid()
    process = Process(pid)
    try:
        regions = process.regions()
        total = sum(size for _, size in regions)
        print(f"pid {pid}: {len(regions)} readable regions, {total/1e6:.0f} MB")
        if arguments.list:
            for base, size in sorted(regions, key=lambda r: -r[1])[:20]:
                print(f"  {base:#018x}  {size/1e6:8.2f} MB")

        arm9 = ARM9.read_bytes()
        base, offset = locate_main_ram(process, regions, arm9)
        report: dict = {"pid": pid}
        if base is None:
            print("could not locate main RAM (two-signature check failed)")
        else:
            address = base + offset
            print(f"main RAM at {address:#018x} (= 0x02000000), confirmed")
            report["main_ram_address"] = address
            if arguments.dump_ram:
                data = process.read(address, MAIN_RAM_SIZE)
                arguments.dump_ram.parent.mkdir(parents=True, exist_ok=True)
                arguments.dump_ram.write_bytes(data or b"")
                print(f"-> {arguments.dump_ram} ({len(data or b''):,} bytes)")

        if arguments.sprites:
            sprites = read_savestate.sprite_triples()
            # One compiled alternation over every sprite signature, so each
            # region is scanned once instead of 200 times.
            signatures: dict[bytes, int] = {}
            for asset_id in sorted(sprites):
                pixels = read_savestate.read_asset(asset_id)
                if not pixels or len(pixels) < SIGNATURE_BYTES:
                    continue
                for start in (0, len(pixels) // 3, len(pixels) // 2):
                    piece = pixels[start : start + 48]
                    if len(piece) == 48 and piece.count(piece[0]) <= read_savestate.FLAT_LIMIT:
                        signatures.setdefault(piece, asset_id)
                        break
            pattern = re.compile(b"|".join(re.escape(s) for s in signatures))
            print(f"\nscanning for {len(signatures)} sprite signatures...")

            hits: dict[int, dict] = {}
            for region_base, size in regions:
                data = process.read(region_base, size)
                if not data:
                    continue
                for match in pattern.finditer(data):
                    asset_id = signatures[match.group()]
                    if asset_id in hits:
                        continue
                    info = sprites[asset_id]
                    hits[asset_id] = {
                        "asset": asset_id,
                        "header": info["header"],
                        "palette": info["palette"],
                        "display": info["display"],
                        "address": region_base + match.start(),
                    }
            print(f"{len(hits)} of {len(sprites)} sprite assets are resident:")
            for hit in sorted(hits.values(), key=lambda h: h["address"]):
                width, height = hit["display"]
                print(f"  asset {hit['asset']:5d}  display {width:3}x{height:<3}  "
                      f"palette {hit['palette']:5d}  at {hit['address']:#018x}")
            report["resident_sprites"] = sorted(hits.values(), key=lambda h: h["asset"])

        if arguments.output:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(json.dumps(report, indent=2) + "\n",
                                        encoding="utf-8")
            print(f"\n-> {arguments.output}")
    finally:
        process.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
