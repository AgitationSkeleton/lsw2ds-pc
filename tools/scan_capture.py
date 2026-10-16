"""Read a recorded session: game states, net messages, and when matches ran.

WHY THIS EXISTS. Five entries in the runbook of questions that need the game
running were answered from recordings that had been sitting in `out/work` the
whole time, without the game running once:

  * The networking premise (step 1): in the cartridge's own two-player match the device
    mask is 0x3, the view mask 3 and the game bound 2, on both machines.
  * `music` (step 6): the byte diagnostics.cpp reads is 17 in a fight, on a
    score screen, walking Mos Eisley and in a menu - it does not name a track.
  * The view index (step 7): every seated player already has a real index.
  * The protocol (step 7b): type 0x0A appears once, on the host, in the
    between-rounds window - which is what it is documented to mean.
  * The HUD premise (step 8): the HUD column re-sorts and the palette follows
    the player, so `player = palette - 4` is required, not just defensible.

Each took a throwaway script, and the next question would have taken
another. The technique is worth more than any one answer, so it lives here.

    python tools/scan_capture.py out/work/archive/gpudump_3matches/inst0

WHAT MAKES IT FAST ENOUGH TO BE USED. `tools/reconstruct_ram.py` replays every
delta from the keyframe on each invocation, which is right for one frame and
impossible for thousands - a 6868-frame capture would replay 23 million block
copies to sample every frame. The deltas are applied ONCE here, incrementally,
sampling as they go. That is the whole trick: it turns "rebuild one frame"
into "watch a session".

WHAT IT REPORTS, and why these three together:

  * THE STATE TIMELINE, from [[0x02102900 + 4] + 4]. statelog.h names 42 as the
    arena match and 44 as the score screen.
  * THE NET MESSAGES, walked exactly as netmsglog_policy.cpp walks them.
  * WHEN THE HUD SLOT COLUMN EXISTS, from the decoded `oam_sub` already in
    each frame's state.json - no RAM needed for that one at all.

The third is what makes the other two mean anything. A message type or a state
id is a number until something brackets it, and the slot column brackets a
match exactly: it appears on the frame the round starts and vanishes on the
frame it ends. Every identification listed above came from lining a number up
against those brackets.

WHAT IT CANNOT DO. Tell you what a handler does with a message, or what a state
draws - only when each occurs. And a pointer chase sampled once per frame will
occasionally catch a tear: one frame of gpudump_3matches reads its state as
539506744 because the manager's object pointer was mid-update. A single frame
is worth nothing; agreement across independent events is the evidence.
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys

RAM_BASE = 0x02000000

# statelog.h
STATE_MANAGER = 0x02102900
STATE_CURRENT = 4
STATE_ID = 4

# netmsglog.h
MSG_OWNER = 0x0210794C
MSG_STATION_STRIDE = 0x344
MSG_SLOT_BASE = 0xA3C
MSG_STATIONS = 4
MSG_MAX_VISITS = 16      # netmsglog_policy.cpp's own cap

# arenahud.cpp's read_rows
SLOT_ORIGIN_Y = 5
SLOT_PITCH = 48
FIRST_SLOT_PALETTE = 4
SLOTS = 4


def frames(directory: str):
    """(number, kind, path) for every RAM keyframe and delta, in order."""
    out = []
    for name in os.listdir(directory):
        m = re.match(r"f(\d+)_ram\.(key|delta)$", name)
        if m:
            out.append((int(m.group(1)), m.group(2),
                        os.path.join(directory, name)))
    out.sort()
    return out


def apply_delta(ram: bytearray, blob: bytes) -> None:
    """`DLTA` | block size u32 | count u32 | then per block:
    offset u32 | length u32 | bytes.

    Documented at tools/reconstruct_ram.py, which is where this format is
    defined; applied in order, the deltas reproduce each frame exactly.
    """
    if blob[:4] != b"DLTA":
        return
    _, count = struct.unpack_from("<II", blob, 4)
    off = 12
    for _ in range(count):
        dst, length = struct.unpack_from("<II", blob, off)
        off += 8
        ram[dst:dst + length] = blob[off:off + length]
        off += length


def u32(ram: bytearray, addr: int) -> int:
    a = addr - RAM_BASE
    if a < 0 or a + 4 > len(ram):
        return 0
    return struct.unpack_from("<I", ram, a)[0]


def state_of(ram: bytearray) -> int:
    obj = u32(ram, STATE_MANAGER + STATE_CURRENT)
    if not (RAM_BASE <= obj < RAM_BASE + len(ram) - 8):
        return -1
    return u32(ram, obj + STATE_ID)


def messages(ram: bytearray, station: int):
    """(type, sequence) for each live slot, walked as the guest walks it."""
    base = MSG_OWNER + station * MSG_STATION_STRIDE + MSG_SLOT_BASE
    slot = 0
    out = []
    for _ in range(MSG_MAX_VISITS):
        entry = u32(ram, base + slot * 4)
        delta = entry & 0xFF
        if delta == 0:                      # TST r0, #0xFF at 0x020B04C8
            break
        out.append(((entry >> 8) & 0xFF, (entry >> 16) & 0xFFFF))
        slot = (slot + delta) & 0xFF        # the cursor wraps in a byte
    return out


def hud_rows(path: str):
    """player -> row from a frame's decoded oam_sub, as read_rows does it."""
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except Exception:
        return None
    rows = {}
    for s in doc.get("oam_sub", []):
        if s.get("w") != 32 or s.get("h") != 32:
            continue
        if not (4 <= s.get("x", -1) <= 6):
            continue
        dy = s.get("y", 0) - SLOT_ORIGIN_Y
        if dy < 0 or dy % SLOT_PITCH:       # mid-slide: no row to report
            continue
        row = dy // SLOT_PITCH
        player = s.get("palette", 0) - FIRST_SLOT_PALETTE
        if 0 <= row < SLOTS and 0 <= player < SLOTS:
            rows[player] = row
    return rows


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[0])
        print("\n  usage: python tools/scan_capture.py <gpudump>/instN")
        return 2
    directory = sys.argv[1]
    fr = frames(directory)
    if not fr or fr[0][1] != "key":
        print(f"no RAM keyframe in {directory}")
        return 1

    ram = bytearray(open(fr[0][2], "rb").read())
    print(f"{directory}: {len(fr)} frames, keyframe f{fr[0][0]:06d}, "
          f"{len(ram)} bytes")

    states, msgs, hud = [], [], []
    prev_state = prev_hud = None
    seen_types = {}
    for num, kind, path in fr:
        if kind == "delta":
            apply_delta(ram, open(path, "rb").read())

        st = state_of(ram)
        if st != prev_state:
            states.append((num, st))
            prev_state = st

        live = {}
        for station in range(2):
            for t, seq in messages(ram, station):
                live.setdefault(station, set()).add(t)
                seen_types.setdefault(t, (num, station, seq))
        if live:
            msgs.append((num, {k: sorted(v) for k, v in live.items()}))

        rows = hud_rows(os.path.join(directory, f"f{num:06d}_state.json"))
        on = bool(rows)
        if on != prev_hud:
            hud.append((num, rows if on else None))
            prev_hud = on

    print("\nHUD slot column (a match is bracketed by these):")
    for num, rows in hud:
        what = ("match: " + str(rows)) if rows else "no column"
        print(f"   f{num:06d}  {what}")

    print(f"\nstate transitions ({len(states)}):")
    for num, st in states:
        print(f"   f{num:06d} -> state {st}")

    print(f"\nnet message types, first sighting ({len(seen_types)}):")
    for t in sorted(seen_types):
        num, station, seq = seen_types[t]
        print(f"   type {t:3d} (0x{t:02X})  f{num:06d} station {station} "
              f"seq {seq}")
    if msgs:
        print(f"\nframes carrying messages ({len(msgs)}):")
        for num, live in msgs[:40]:
            print(f"   f{num:06d}  {live}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
