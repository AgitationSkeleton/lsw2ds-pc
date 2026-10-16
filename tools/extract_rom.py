"""Unpack, from your ROM, exactly what port/build.sh needs.

    py -3 tools/extract_rom.py                 # rom/*.nds -> out/raw
    py -3 tools/extract_rom.py --rom X.nds --output DIR

The build lifts four programs and exports the game's fonts. None of their inputs
is committed - they are the game - so they are cut out of your ROM here, the
first time the build runs:

    out/raw/arm7.bin                      the ARM7 program       (tools/nds_extract.py)
    out/raw/arm9_section_0_02000000.bin   ARM9 main, unpacked    (BLZ, then the autoload list)
    out/raw/arm9_section_1_01ff8000.bin   ARM9 ITCM
    out/raw/arm7_wram_037f8000.bin        the ARM7's WRAM copy   (tools/extract_arm7_sections.py)
    out/raw/archive/entries/NNNN.*        the five font assets   (the archive's own decoder)

Each output is byte-identical to what the full research tools produce; the
SHA-1 of every one was checked against them.

ONLY THE FONTS FROM THE ARCHIVE. A complete unpack of the game's archive
(tools/lsw2_archive_run.py) takes about half an hour, because each compressed
entry is expanded by running the game's own decompressor from ITCM under an
emulated CPU. The build needs five entries of the 5,548, so only those are
expanded. Run the full tool when you want every asset.

NEEDS ndspy AND unicorn, which are installed on first use into
tools/vendor/python (ignored by git) with this interpreter's pip - a Windows
CPython from python.org, not MSYS2's, because unicorn ships wheels only for it.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
VENDOR = TOOLS / "vendor" / "python"
# The same pins as tools/install_vendor.ps1.
REQUIREMENTS = [
    "git+https://github.com/RoadrunnerWMC/ndspy.git@629c18e3b555dd33da71c741912fc4b814ef7492",
    "unicorn==2.1.4",
]
FONT_ASSETS = (2298, 2299, 2300, 2374, 3442)   # tools/export_custom_fonts.py FONTS
ROM_SHA1 = "7c1afa070797945ba5a2577c7ddfded5cf2fc443"


def ensure_vendor() -> None:
    sys.path.insert(0, str(VENDOR))
    try:
        import ndspy.rom  # noqa: F401
        import unicorn  # noqa: F401
        return
    except ImportError:
        pass
    print(f"installing ndspy and unicorn into {VENDOR.relative_to(ROOT)}", flush=True)
    VENDOR.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--upgrade",
                    "--target", str(VENDOR), *REQUIREMENTS], check=True)
    importlib.invalidate_caches()
    import ndspy.rom  # noqa: F401
    import unicorn  # noqa: F401


def load_tool(name: str):
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rom", type=Path, help="default: the .nds in rom/")
    ap.add_argument("--output", type=Path, default=ROOT / "out" / "raw")
    args = ap.parse_args()

    rom = args.rom
    if rom is None:
        roms = sorted((ROOT / "rom").glob("*.nds"))
        if not roms:
            print("no ROM in rom/", file=sys.stderr)
            return 1
        rom = roms[0]
    data = rom.read_bytes()
    if hashlib.sha1(data).hexdigest() != ROM_SHA1:
        print(f"note: {rom.name} is not the stock USA cartridge "
              f"(SHA-1 {hashlib.sha1(data).hexdigest()})", flush=True)
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    # 1. The container: arm7.bin among others.
    subprocess.run([sys.executable, str(TOOLS / "nds_extract.py"), str(rom), "-o", str(out)],
                   check=True, stdout=subprocess.DEVNULL)
    print("   cartridge unpacked")

    ensure_vendor()
    import ndspy.codeCompression
    import ndspy.rom

    # 2. The ARM9: BLZ-decompressed, then split by its autoload list.
    nds = ndspy.rom.NintendoDSRom(data)
    (out / "arm9_decompressed.bin").write_bytes(ndspy.codeCompression.decompress(nds.arm9))
    for i, section in enumerate(nds.loadArm9().sections):
        (out / f"arm9_section_{i}_{section.ramAddress:08x}.bin").write_bytes(bytes(section.data))
    print("   ARM9 unpacked and split")

    # 3. The ARM7's WRAM copy, by the research tool's own measured mapping.
    arm7 = load_tool("extract_arm7_sections")
    arm7.ROOT, arm7.OUT = ROOT, out
    if arm7.main() not in (0, None):
        print("extract_arm7_sections failed", file=sys.stderr)
        return 1
    print("   ARM7 WRAM copy cut")

    # 4. The fonts, expanded with the game's own decoder.
    fast = load_tool("lsw2_archive_fast")
    core = fast.core
    core.SOURCE_ADDRESS, core.OUTPUT_ADDRESS = 0x10000000, 0x20000000
    core.STACK_ADDRESS, core.RETURN_ADDRESS = 0x30000000, 0x40000000
    archive = core.read_rom_bin(rom)
    count = int.from_bytes(archive[0:4], "little") // 4
    offsets = [int.from_bytes(archive[4 * i:4 * i + 4], "little") for i in range(count)]
    decoder = core.Type6Decoder(core.load_itcm(rom))
    entries = out / "archive" / "entries"
    entries.mkdir(parents=True, exist_ok=True)
    for asset_id in FONT_ASSETS:
        packed = archive[offsets[asset_id]:offsets[asset_id + 1]]
        expanded = packed
        if len(packed) >= 16 and ((packed[0] >> 4) & 7) == 6:
            expanded = decoder.decode(packed)
        ext = core.NITRO_EXTENSIONS.get(expanded[:4], "bin")
        (entries / f"{asset_id:04d}.{ext}").write_bytes(expanded)
    print(f"   {len(FONT_ASSETS)} font assets expanded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
