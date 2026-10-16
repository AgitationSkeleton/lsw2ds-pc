#!/usr/bin/env python3
"""Snap the Free Play screen repeatedly while someone navigates it live.

One dump.request at a time, answered on the emulator's 30-frame poll, filed
under a sequential name. Pages are identified afterwards by content rather than
by guessing when the taps landed.
"""
from __future__ import annotations
import shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUMP = ROOT / "out/work/gpudump_select"
SHOTS = DUMP / "shots"


def snap(index: int) -> bool:
    done = DUMP / "dump.done"
    if done.exists():
        done.unlink()
    (DUMP / "dump.request").write_text("", encoding="utf-8")
    for _ in range(40):
        if done.exists():
            break
        time.sleep(0.1)
    else:
        return False
    target = SHOTS / ("%03d" % index)
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    src = DUMP / "inst0"
    # Only what is needed to reconstruct the touch screen, to keep this fast
    # enough to run in a tight loop: both screens, the sub-engine banks, the
    # palette and the state.
    for name in ["screen0.bin", "screen1.bin", "palette.bin", "state.json",
                 "vram_C.bin", "vram_I.bin", "vram_H.bin"]:
        path = src / name
        if path.exists():
            shutil.copyfile(path, target / name)
    return True


def main() -> int:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    SHOTS.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + seconds
    index = 0
    while time.time() < deadline:
        if snap(index):
            index += 1
        time.sleep(0.35)
    print("%d snapshots -> %s" % (index, SHOTS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
