#!/usr/bin/env python3
"""Replay a keyframe+delta capture back to full buffers at any frame.

GPUDump writes main RAM and the two interesting VRAM banks as a keyframe
(".key") followed by deltas (".delta") - "DLTA", block size, block count, then
each changed block as [offset][length][bytes]. A frame with nothing changed
writes no file at all, so a missing delta means "same as the frame before".

This walks that chain and hands back the reconstructed buffer, which is what
makes the capture searchable: a value that tracks the on-screen score can be
found by asking which addresses hold the right number at every sampled frame.

    from tools.replay_dump import Replay
    r = Replay("out/work/gpudump_score/inst0", "ram")
    for frame in r.frames:
        buffer = r.at(frame)
"""

from __future__ import annotations

import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FRAME_RE = re.compile(r"^f(\d+)_(.+?)\.(key|delta)$")


class Replay:
    """Sequential reconstruction of one buffer kind out of one instance dir."""

    def __init__(self, directory: str | Path, kind: str = "ram") -> None:
        self.directory = Path(directory)
        self.kind = kind
        self._steps: dict[int, tuple[str, Path]] = {}

        for path in self.directory.iterdir():
            match = FRAME_RE.match(path.name)
            if match is None or match.group(2) != kind:
                continue
            self._steps[int(match.group(1))] = (match.group(3), path)

        if not self._steps:
            raise FileNotFoundError("no %s frames in %s" % (kind, self.directory))

        self.frames = sorted(self._steps)
        self._buffer: bytearray | None = None
        self._position = -1

    def _apply(self, path: Path) -> None:
        raw = path.read_bytes()
        if raw[:4] != b"DLTA":
            raise ValueError("%s is not a delta" % path.name)
        count = struct.unpack_from("<I", raw, 8)[0]
        at = 12
        for _ in range(count):
            offset, span = struct.unpack_from("<II", raw, at)
            at += 8
            self._buffer[offset:offset + span] = raw[at:at + span]
            at += span

    def at(self, frame: int) -> bytes:
        """The buffer as of `frame`. Frames must be requested in order."""
        if frame < self._position:
            self._buffer = None
            self._position = -1

        for step in self.frames:
            if step <= self._position or step > frame:
                continue
            kind, path = self._steps[step]
            if kind == "key":
                self._buffer = bytearray(path.read_bytes())
            elif self._buffer is None:
                # A delta before any keyframe cannot be replayed.
                continue
            else:
                self._apply(path)
            self._position = step

        if self._buffer is None:
            raise RuntimeError("no keyframe at or before frame %d" % frame)
        return bytes(self._buffer)


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--kind", default="ram")
    arguments = parser.parse_args()

    replay = Replay(arguments.directory, arguments.kind)
    print("%d frames of %s, %d..%d"
          % (len(replay.frames), arguments.kind,
             replay.frames[0], replay.frames[-1]))
    print("size %d bytes" % len(replay.at(replay.frames[0])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
