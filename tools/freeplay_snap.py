#!/usr/bin/env python3
"""Take one on-demand full dump from the running Free Play session.

GPUDump polls for a file named dump.request every 30 frames and answers it with
a complete snapshot - both framebuffers, all VRAM banks, palette, OAM, and the
2D/3D state - then writes dump.done. This drops the request, waits for the
answer, and files the result under a name, so a run can be walked page by page:

    python tools/freeplay_snap.py page1_rebel_blasters
    python tools/freeplay_snap.py customise_hats
"""
from __future__ import annotations
import shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUMP = ROOT / "out/work/gpudump_freeplay"


def main() -> int:
    label = sys.argv[1] if len(sys.argv) > 1 else "snap"
    done = DUMP / "dump.done"
    if done.exists():
        done.unlink()
    (DUMP / "dump.request").write_text("", encoding="utf-8")
    for _ in range(100):
        if done.exists():
            break
        time.sleep(0.2)
    else:
        print("no dump.done after 20s - is the emulator running?")
        return 1
    target = DUMP / label
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for inst in sorted(DUMP.glob("inst*")):
        shutil.copytree(inst, target / inst.name)
    print("captured %s" % target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
