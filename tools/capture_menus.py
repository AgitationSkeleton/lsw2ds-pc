#!/usr/bin/env python3
"""Take full GPU dumps repeatedly and archive each, for capturing menus.

Menus cannot be reconstructed from the light periodic record: that holds only
registers, OAM, palette and two VRAM banks, whereas `reconstruct_screen.py`
needs every bank to resolve the mapping. Full dumps have all of them, but each
overwrites the last - so this requests one every few seconds and files it under
its own directory.

Screens that do not change produce near-identical dumps, so each is compared
against the previous one and skipped when nothing moved. That keeps a few
minutes of menu navigation down to one directory per distinct screen instead of
one per request.

Nothing needs restarting: the request mechanism is live whenever
MELONDS_GPUDUMP_DIR is set, independently of the periodic recording.

Run:
    python tools/capture_menus.py --seconds 240
    python tools/capture_menus.py --interval 3 --out out/work/menus
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUMP_DIR = ROOT / "out/work/gpudump"


def request(directory: Path, timeout: float = 25.0) -> int:
    done = directory / "dump.done"
    done.unlink(missing_ok=True)
    (directory / "dump.request").write_text("go\n", encoding="utf-8")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if done.exists():
            try:
                return int(done.read_text().strip() or 0)
            except ValueError:
                return 0
        time.sleep(0.1)
    return 0


# The files a full dump writes. Anything else in the directory - notably the
# f000000_* files left by a previous periodic-recording session - is stale and
# must be ignored, or the signature is computed from leftovers, never changes,
# and the capture silently records one screen for a whole session.
DUMP_FILES = (
    "state.json", "palette.bin", "oam.bin", "polygons.json",
    "screen0.bin", "screen1.bin",
    "vram_A.bin", "vram_B.bin", "vram_C.bin", "vram_D.bin", "vram_E.bin",
    "vram_F.bin", "vram_G.bin", "vram_H.bin", "vram_I.bin",
)
SIGNATURE_FILES = ("state.json", "palette.bin", "vram_C.bin", "vram_I.bin",
                   "oam.bin", "screen1.bin")


def screen_signature(instance: Path) -> str:
    """Hash the things that define what is on screen.

    Returns "" if any required file is missing, so a half-written or stale
    directory is skipped rather than treated as a screen.
    """
    parts = []
    for name in SIGNATURE_FILES:
        path = instance / name
        if not path.exists():
            return ""
        parts.append(hashlib.md5(path.read_bytes()).hexdigest())
    return "|".join(parts)


def archive(source: Path, target: Path) -> bool:
    """Copy just the dump files, leaving stale periodic records behind."""
    target.mkdir(parents=True, exist_ok=False)
    copied = 0
    for name in DUMP_FILES:
        path = source / name
        if path.exists():
            shutil.copy2(path, target / name)
            copied += 1
    return copied >= len(SIGNATURE_FILES)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump-dir", type=Path, default=DUMP_DIR)
    parser.add_argument("--out", type=Path, default=ROOT / "out/work/menus")
    parser.add_argument("--interval", type=float, default=3.0)
    parser.add_argument("--seconds", type=float, default=240.0)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)

    print(f"capturing distinct screens for {arguments.seconds:.0f}s "
          f"every {arguments.interval:.0f}s -> {arguments.out}")
    previous = ""
    kept = 0
    deadline = time.time() + arguments.seconds
    index = 0
    while time.time() < deadline:
        count = request(arguments.dump_dir)
        if not count:
            print("  (no response - is the emulator running?)")
            time.sleep(arguments.interval)
            continue
        source = arguments.dump_dir / "inst0"
        signature = screen_signature(source)
        if signature and signature != previous:
            previous = signature
            # Never reuse or delete a directory. Two capture loops running at
            # once - easily done by starting a second before the first has
            # finished - would otherwise each count from zero and delete each
            # other's screens, which has already destroyed one session's
            # navigation. Claim the next free index instead.
            while True:
                target = arguments.out / f"screen{kept:03d}"
                if not target.exists():
                    break
                kept += 1
            if archive(source, target):
                kept += 1
                print(f"  [{index:3d}] new screen -> {target.name}")
            else:
                print(f"  [{index:3d}] incomplete dump, skipped")
        index += 1
        time.sleep(arguments.interval)

    print(f"\n{kept} distinct screens captured from {index} dumps")
    print(f"reconstruct each with:  python tools/reconstruct_screen.py --dir {arguments.out}/screenNNN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
