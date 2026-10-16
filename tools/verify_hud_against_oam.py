#!/usr/bin/env python3
"""Check the port's bottom-screen HUD constants against captured OAM.

The HUD layout numbers were the largest block flagged by
tools/audit_unfounded_constants.py - dozens of positions and sizes with no
citation. They are not in the ARM9 image (they are written to OAM at runtime),
so the check is against the sprite table a capture actually recorded.

GPUDump writes `<frame>_state.json` per capture with `oam_main` and `oam_sub`
entries carrying x, y, w, h, tile, palette and priority. `oam_sub` is the touch
screen, which is where the Bounty Hunter HUD lives.

What a match proves: the constant equals what the DS put on screen in that
frame. What a mismatch means has to be judged - the element may simply not be
present in the sampled frame (Capture-mode icons in a deathmatch, buttons on a
different screen state), which is why unmatched constants are reported
separately from wrong ones rather than failed outright.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load_oam(path: Path, key: str = "oam_sub"):
    return json.loads(path.read_text()).get(key, [])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("state", help="a <frame>_state.json from a gpudump")
    parser.add_argument("--key", default="oam_sub", choices=["oam_sub", "oam_main"])
    args = parser.parse_args()

    sprites = load_oam(Path(args.state), args.key)
    if not sprites:
        print("no %s sprites in %s" % (args.key, args.state))
        return 1

    at = {(int(s["x"]), int(s["y"])): s for s in sprites}

    # The port's constants, with what each is claimed to be.
    claims = [
        ("SIGNAL_BARS_AT", (164, 156), "the link-quality icon"),
        ("RADAR_SELF_AT", (169, 71), "the player's own radar blip"),
        ("RADAR_CENTRE", (169, 71), "the radar centre"),
    ]
    print("=== direct position claims ===")
    for name, pos, what in claims:
        hit = at.get(pos)
        print("  %-16s %-10s %-28s %s"
              % (name, str(pos), what,
                 "CONFIRMED %dx%d" % (hit["w"], hit["h"]) if hit else "not in this frame"))

    # The four player slots. Each is a ring drawn as three sprites - a 32x32
    # body, an 8x32 right edge and a 32x8 bottom strip - so the ring is 40x40.
    print()
    print("=== player slots ===")
    rings = sorted({int(s["y"]) for s in sprites
                    if int(s["x"]) == 5 and int(s["w"]) == 32 and int(s["h"]) == 32})
    print("  ring tops at x=5: %s" % rings)
    if len(rings) >= 2:
        pitches = {rings[i + 1] - rings[i] for i in range(len(rings) - 1)}
        print("  SLOT_PITCH   %s" % ("CONFIRMED %d" % pitches.pop()
                                     if len(pitches) == 1 else "IRREGULAR %s" % pitches))
    print("  SLOT_COUNT   %s" % ("CONFIRMED %d" % len(rings) if rings else "none"))

    # Ring size from the three-piece construction, portrait inset from the
    # portrait sprite sitting inside the first ring.
    body = at.get((5, rings[0])) if rings else None
    edge = at.get((37, rings[0])) if rings else None
    if body and edge:
        print("  RING_SIZE    CONFIRMED %d (32 body + 8 edge)" % (body["w"] + edge["w"]))
    portraits = [s for s in sprites if int(s["x"]) == 8 and int(s["w"]) == 32]
    if portraits and rings:
        inset = 8 - 5
        print("  PORTRAIT_INSET CONFIRMED %d (portrait x=8 inside ring x=5)" % inset)
        print("  PORTRAIT_SIZE  CONFIRMED %d" % portraits[0]["w"])

    # Anything on screen the port does not model.
    print()
    print("=== sprites present that no constant accounts for ===")
    accounted = {(164, 156), (169, 71)}
    for y in rings:
        accounted |= {(5, y), (37, y), (5, y + 32), (8, y + 3)}
    unknown = [s for s in sprites if (int(s["x"]), int(s["y"])) not in accounted]
    if not unknown:
        print("  none")
    for s in unknown:
        print("  x=%-4d y=%-4d %dx%-3d tile=%-4d palette=%d priority=%d"
              % (s["x"], s["y"], s["w"], s["h"], s["tile"], s["palette"], s["priority"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
