"""Freeze a capture's main RAM to a flat file the recomp core can load.

THE ORACLE NEEDS A STARTING STATE. The recompiled core cannot boot - the port's
scope skips boot, and reaching game code from the reset vector would mean building
DMA, timers, the interrupt controller and VRAM first. So state comes from
outside: a recorded session's main RAM at a chosen frame, which is real memory
from the real game running on the real hardware.

tools/reconstruct_ram.py already rebuilds that from a keyframe plus deltas. This
writes the result out as a plain 4 MB image plus a small JSON manifest, so a C
test can `fread` it without needing Python at run time.

    python tools/export_ram_snapshot.py --capture out/work/gpudump_walk_mos/inst0 \
        --frame 12000 --out out/work/ramsnap/mos_f12000
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import reconstruct_ram as rr  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", required=True)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    directory = (ROOT / args.capture) if not Path(args.capture).is_absolute() \
        else Path(args.capture)
    ram, frame = rr.rebuild(directory, args.frame)

    # DTCM, WHEN THE CAPTURE HAS IT. A main-RAM-only snapshot cannot drive a
    # frame: FUN_020016C0 pops a stack whose depth is at 0x027E02A0, inside
    # DTCM, and a zero-filled DTCM makes that index underflow immediately.
    # Older captures predate the instrumentation that records it, so its
    # absence is reported rather than treated as an error.
    dtcm = None
    dtcm_frame = None
    try:
        dtcm, dtcm_frame = rr.rebuild(directory, args.frame, "dtcm")
    except SystemExit:
        dtcm = None

    out = (ROOT / args.out) if not Path(args.out).is_absolute() else Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    bin_path = out.with_suffix(".bin")
    bin_path.write_bytes(bytes(ram))
    if dtcm is not None:
        dtcm_path = out.parent / (out.name + "_dtcm.bin")
        dtcm_path.write_bytes(bytes(dtcm))
        print("dtcm  frame %d -> %s (%d bytes at 0x%08X)"
              % (dtcm_frame, dtcm_path.relative_to(ROOT), len(dtcm),
                 rr.DTCM_BASE))
    else:
        print("dtcm  NOT IN THIS CAPTURE - a frame cannot be driven from it; "
              "re-record with the patched melonDS")

    manifest = {
        "schema": 1,
        "note": "Main RAM from a recorded session, for the recomp core's "
                "differential harness. Real memory from the real game.",
        "capture": str(directory),
        "requested_frame": args.frame,
        "actual_frame": frame,
        "base": "0x%08X" % rr.MAIN_RAM_BASE,
        "bytes": len(ram),
        "image": bin_path.name,
        "dtcm": (out.name + "_dtcm.bin") if dtcm is not None else None,
        "dtcm_base": "0x%08X" % rr.DTCM_BASE,
        "dtcm_frame": dtcm_frame,
    }
    out.with_suffix(".json").write_text(json.dumps(manifest, indent=1),
                                        encoding="utf-8")
    print("frame %d -> %s (%d bytes at 0x%08X)"
          % (frame, bin_path.relative_to(ROOT), len(ram), rr.MAIN_RAM_BASE))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
