#!/usr/bin/env python3
"""Recover the DS camera pose from the captured view matrix.

Vertices in the polygon dump are already projected, so the camera cannot be read
off them. GPUDump now also captures the matrix stack, and PosMatrix at render
time IS the view transform for world-space geometry - which makes the camera
recoverable, and with it a port-versus-DS pixel comparison that means something
for the 3D world the way it already does for the UI.

The DS stores matrices column-major in 20.12 fixed point. A view matrix is
R * T(-eye), so:

    eye   = -R_transpose * t
    basis =  R_transpose      (camera right / up / forward in world space)

    python tools/decode_camera.py out/work/gpudump_fog/inst0/f007200_state.json
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXED = 4096.0


def columns(flat: list[int]) -> list[list[float]]:
    """Column-major 4x4 as a list of 4 columns of 4 floats."""
    return [[flat[c * 4 + r] / FIXED for r in range(4)] for c in range(4)]


def decode(state: dict) -> dict:
    pos = columns(state["pos_matrix"])
    rotation = [column[:3] for column in pos[:3]]   # 3 columns of 3
    translation = pos[3][:3]

    # eye = -R^T * t. R^T's rows are R's columns, so this is a dot product of
    # each rotation column with the translation.
    eye = [-sum(rotation[axis][k] * translation[k] for k in range(3))
           for axis in range(3)]

    # R^T rows are the camera's world-space axes.
    right = [rotation[0][k] for k in range(3)]
    up = [rotation[1][k] for k in range(3)]
    back = [rotation[2][k] for k in range(3)]
    forward = [-value for value in back]

    pitch = math.degrees(math.asin(max(-1.0, min(1.0, -forward[1]))))
    yaw = math.degrees(math.atan2(forward[0], forward[2]))

    out = {"eye": eye, "forward": forward, "right": right, "up": up,
           "pitch_deg": pitch, "yaw_deg": yaw}

    if "proj_matrix" in state:
        proj = columns(state["proj_matrix"])
        m0, m5 = proj[0][0], proj[1][1]
        if m0 and m5:
            out["fov_vertical_deg"] = 2 * math.degrees(math.atan(1.0 / m5))
            out["fov_horizontal_deg"] = 2 * math.degrees(math.atan(1.0 / m0))
            out["projection_aspect"] = m5 / m0
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("state", type=Path, nargs="+")
    arguments = parser.parse_args()

    for path in arguments.state:
        state = json.loads(path.read_text(encoding="utf-8"))
        if "pos_matrix" not in state:
            print("%s: no pos_matrix - captured before the matrix dump existed"
                  % path.name)
            continue
        result = decode(state)
        print("%s" % path.name)
        print("  eye      (%9.3f, %9.3f, %9.3f)" % tuple(result["eye"]))
        print("  forward  (%9.4f, %9.4f, %9.4f)" % tuple(result["forward"]))
        print("  pitch %7.2f deg   yaw %7.2f deg" %
              (result["pitch_deg"], result["yaw_deg"]))
        if "fov_vertical_deg" in result:
            print("  fov  vertical %6.2f  horizontal %6.2f  aspect %.4f"
                  % (result["fov_vertical_deg"], result["fov_horizontal_deg"],
                     result["projection_aspect"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
