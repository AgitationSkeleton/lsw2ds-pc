"""Export the DS sound-effect event table.

WHERE THIS LIVES IN THE ROM
---------------------------
The game has ONE sound-effect dispatch and it resolves an event id through two
tables.

    0x020B8ECC   the default parameter block: memset(sp,0,0xC) then
                 `mov r1,#7 / strb r1,[sp]`, so byte 0 is 7 and the rest zero
    0x020B8F70   the wrapper - 100 BL callers. Builds that block, forwards the
                 owner tag, and tail-calls the body
    0x020B8FA4   the body

    0x02100A74   table 1, stride 4, one row per event id:
                     +0x00 u16 variant count
                     +0x02 u16 base index into table 2
                 loaded by 0x020B8FB0 (lit 0x020B905C) and 0x020B8FBC
                 (lit 0x020B9060 = 0x02100A76)

    0x02100D98   table 2, stride 4, one row per variant:
                     +0x00 u16 sample-descriptor index
                     +0x02 u16 playback rate in Hz, 0 meaning "use the
                           descriptor's own"
                 loaded by 0x020B8FE0 (lit 0x020B9064). BOTH halves are read -
                 0x020B9030 `ldrh r0,[r4]` and 0x020B9034 `ldrh r1,[r4,#2]`

    index math:  `lsl r2,r0,#2` / `ldrh r0,[r1,r2]` / `ldrh r4,[r1,r2]` then
                 `add r0,r4,r0` / `add r4,r1,r0,lsl #2`
                 so entry = 0x02100D98 + (base + variant) * 4

    0x020B8FC0   `cmp r0,#1 / movls r0,#0 / bls` - a single-variant event
                 short-circuits and draws NO random number
    0x02062B10   otherwise, uniform on [0, count-1]

    descriptor:  *(0x0210DB84 + 0x7C) + index * 0x24, unpacked at 0x020BAAAC

THE 201 BOUND, three independent ways
-------------------------------------
1. Geometry. 0x02100A74 + 201*4 = 0x02100D98 exactly - table 1 ends where
   table 2 begins.
2. Run-length closure. base[i+1] == base[i] + count[i] holds for every i, and
   closes at 232 total variants, which is table 2's own row count. Reading a
   202nd row breaks the invariant.
3. There is NO id guard in the dispatch: no `cmp` on r0 exists anywhere between
   0x020B8FA4 and the `ldrh` at 0x020B8FB8. An out-of-range id reads out of
   bounds on hardware.

0xC9 (201) is therefore a CALLER-side sentinel, not a dispatch check. A scan for
`cmp Rn,#0xC9` finds exactly eleven sites - 0x02004130, 0x02005680, 0x0200B0B4,
0x02014F84, 0x02018CC8, 0x02019574, 0x0202FB6C, 0x02037B84, 0x02068274,
0x0209FAEC, 0x0209FB04 - and only the last two are range tests (BHS); the rest
are equality tests that skip the call.

Usage:  python tools/export_sound_events.py
"""

from __future__ import annotations

import collections
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000

EVENT_TABLE = 0x02100A74
VARIANT_TABLE = 0x02100D98
EVENT_COUNT = 201

MANIFEST = ROOT / "out" / "snd" / "sounddata_manifest.json"
OUT = ROOT / "config" / "sound_events.json"

# The sentinel callers use to mean "no sound" / "stop the current loop".
NO_SOUND = 0xC9


def main() -> int:
    image = IMAGE.read_bytes()

    def u16(addr: int) -> int:
        return struct.unpack_from("<H", image, addr - BASE)[0]

    # --- table 1 ---------------------------------------------------------
    events = []
    for event_id in range(EVENT_COUNT):
        row = EVENT_TABLE + event_id * 4
        events.append({"count": u16(row), "base": u16(row + 2)})

    # Invariant: the table is one contiguous run.
    total = 0
    for i, e in enumerate(events):
        if e["base"] != total:
            raise SystemExit(
                "run-length broken at event %d: base %d, expected %d"
                % (i, e["base"], total))
        total += e["count"]
    if total != 232:
        raise SystemExit("variants total %d, expected 232" % total)

    # And reading one row past the end must BREAK it, which is what fixes the
    # bound at 201 rather than leaving it a guess.
    beyond = {"count": u16(EVENT_TABLE + EVENT_COUNT * 4),
              "base": u16(EVENT_TABLE + EVENT_COUNT * 4 + 2)}
    if beyond["base"] == total:
        raise SystemExit("row 201 continues the run - the 201 bound is wrong")

    # --- table 2 ---------------------------------------------------------
    variants = []
    for i in range(total):
        row = VARIANT_TABLE + i * 4
        variants.append({"descriptor": u16(row), "rate": u16(row + 2)})

    rates = collections.Counter(v["rate"] for v in variants)
    if set(rates) != {32768}:
        raise SystemExit("playback rates are not all 32768: %s" % dict(rates))

    indices = sorted({v["descriptor"] for v in variants})
    if indices != list(range(128, 360)):
        raise SystemExit(
            "descriptor indices are not the bijection onto 128..359 "
            "(got %d distinct, %d..%d)" % (len(indices), indices[0], indices[-1]))

    # --- join to the sample descriptors ----------------------------------
    descriptors = []
    if MANIFEST.exists():
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        descriptors = document.get("sample_descriptors", [])

    def describe(index: int) -> dict:
        out = {"descriptor_index": index}
        if index < len(descriptors):
            d = descriptors[index]
            out["sample_id"] = d.get("sample_id")
            out["looping"] = bool(d.get("looping", False))
            out["sample_rate"] = d.get("sample_rate")
            out["wav"] = "out/snd/samples/sample_%03d.wav" % int(d.get("sample_id", -1))
        return out

    rows = []
    for event_id, e in enumerate(events):
        picks = [describe(variants[e["base"] + k]["descriptor"])
                 for k in range(e["count"])]
        rows.append({
            "id": event_id,
            "count": e["count"],
            "base": e["base"],
            # 32768 for every row in this ROM; kept per-variant because the
            # field exists and 0 would mean "use the descriptor's own".
            "playback_rate": variants[e["base"]]["rate"] if e["count"] else 0,
            "variants": picks,
        })

    multi = [r for r in rows if r["count"] > 1]
    looping = [r for r in rows if any(v.get("looping") for v in r["variants"])]
    empty = [r for r in rows if r["count"] == 0]

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "dispatch": "0x020B8F70",
            "body": "0x020B8FA4",
            "event_table": "0x%08X" % EVENT_TABLE,
            "variant_table": "0x%08X" % VARIANT_TABLE,
            "event_count": EVENT_COUNT,
            "variant_count": total,
            "no_sound_sentinel": NO_SOUND,
        },
        "note": __doc__.strip(),
        "events": rows,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("events            %d  (0..%d)" % (len(rows), len(rows) - 1))
    print("variants          %d  (run-length closes exactly)" % total)
    print("row %d breaks the run: base %d vs expected %d  <- fixes the bound"
          % (EVENT_COUNT, beyond["base"], total))
    print("playback rates    %s" % dict(rates))
    print("descriptor range  %d..%d, %d distinct - exact bijection"
          % (indices[0], indices[-1], len(indices)))
    print("events with alternates  %d  %s" % (len(multi), [r["id"] for r in multi]))
    print("events resolving to a looping descriptor  %d" % len(looping))
    print("events with zero variants  %d  %s" % (len(empty), [r["id"] for r in empty]))
    if descriptors:
        missing = [v["wav"] for r in rows for v in r["variants"]
                   if "wav" in v and not (ROOT / v["wav"]).exists()]
        print("resolved wavs missing on disk  %d" % len(missing))
    else:
        print("!! %s not found - sample ids not joined" % MANIFEST)
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
