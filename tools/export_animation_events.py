"""Decode the ANIMATION EVENT TRACKS - where the game's sounds actually live.

WHY THIS EXISTS. The port was silent apart from the jetpack, and the reason was
not a missing binding table: it is that MOST SOUNDS ARE NOT TRIGGERED BY CODE AT
ALL. They are events on the animation, fired at a specific frame. Searching the
ARM9 for constant sound ids finds 29 call sites - the jetpack, the grapple, the
punch - and none of them is a footstep or a blaster shot, because those are in
the animation data.

THE TRACK. Every descriptor record is (state, anim, attached, events, queued),
and `events` is an archive asset holding 6-BYTE RECORDS:

    +0x00  u16  frame
    +0x02  u16  opcode
    +0x04  u16  param

terminated by FFFF FFFF FFFF. The layout is the dispatcher's own, from the ITCM
routine at 0x01FFAD3C - a function that did not exist in this project's Ghidra
corpus until the ITCM section was imported:

    iVar5 = *(int *)(param_1 + 0xa0);                 the track
    iVar4 = *(u8 *)(param_1 + 0xb1) * 6;              CURSOR * 6
    if (*(u16 *)(iVar5 + iVar4) <= currentFrame)      +0 is the FRAME
        FUN_0209FA28(param_1, *(u16 *)(iVar5 + iVar4 + 2),   +2 OPCODE
                              *(u16 *)(iVar5 + iVar4 + 4),   +4 PARAM
                     param_3);

THE OPCODES, from FUN_0209FA28's switch:

    0   clear bit 0 / set bit 1 of +0x7A
    1   stop the track
    2   PLAY A POSITIONAL SOUND, param, if param < 0xC9   (FUN_020B95BC)
    3   PLAY A SOUND, param, if param < 0xC9              (FUN_020B8F70)
    6   weapon draw/holster, FUN_02005654(actor, param)
    7   set +0xB2 bit 0x04          8   clear it
    9   set bit 0x08               10   set bit 0x100      11  clear bit 0x100
    12  stop                       13   conditional stop
    14  set 0x10                   15   set 0x20           16  set 0x40
    17  set +0x7A bit 1
    19  latch +0xB0 to param when it is -1     20  loop counter, decrement
    21  SHOW attachment slot param  (FUN_0200C848)
    22  HIDE attachment slot param  (FUN_0200C834)
    24  FIRE THE WEAPON from muzzle `param`
    25  FUN_02005698(actor)        26/27  FUN_02004B68 with 9 / 10
    28  FUN_020A95D4(..., 2, param)

`0xC9` = 201 is the no-sound sentinel, one past the 201 sound events - the same
sentinel the projectile-kind table uses at its +0x09.

TWO THINGS THIS SETTLES BEYOND SOUND.

**Opcode 24 is the shot.** It calls FUN_02005854 with the action row's +0x04
projectile kind and passes `param` as the MUZZLE SELECTOR. BlstrShootGun's track
is a single record - frame 1, opcode 24, param 5 - so the bolt leaves muzzle 5
one frame into the fire animation. That is an independent corroboration of the
muzzle decode, which was read out of FUN_02005854 from the other end.

**Opcodes 21 and 22 are the weapon show/hide**, on slots 4 and 5 - the same two
slots the dual-wield attach uses.

Usage:  python tools/export_animation_events.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
CATALOG = ROOT / "out" / "raw" / "archive" / "catalog.json"
SETS = ROOT / "config" / "animation_descriptor_sets.json"
SOUND_EVENTS = ROOT / "config" / "sound_events.json"
OUT = ROOT / "config" / "animation_events.json"

BASE = 0x02000000
# The descriptor-array region: everything between the end of the code and the
# descriptor-set table.
REGION_LO = 0x020E5C74
REGION_HI = 0x020EB7B8
NO_SOUND = 0xC9
TERMINATOR = 0xFFFF

OPCODES = {
    0: "flag_7a_swap", 1: "stop", 2: "sound_positional", 3: "sound",
    4: "nop", 5: "nop", 6: "weapon_toggle",
    7: "set_b2_04", 8: "clear_b2_04", 9: "set_b2_08",
    10: "set_b2_100", 11: "clear_b2_100", 12: "stop",
    13: "stop_if", 14: "set_b2_10", 15: "set_b2_20", 16: "set_b2_40",
    17: "set_7a_02", 18: "nop", 19: "latch_b0", 20: "loop_b0",
    21: "attach_show", 22: "attach_hide", 23: "nop",
    24: "fire", 25: "call_02005698", 26: "call_b68_9", 27: "call_b68_10",
    28: "call_a95d4",
}
SOUND_OPCODES = {2, 3}

SKIP_NAMES = {"BMD0", "MDL0", "BCA0", "JNT0", "TEX0", "BTA0", "SRT0", "BTP0",
              "PAT0", "BTX0"}


def read_track(asset: int) -> list[dict] | None:
    matches = list(ENTRIES.glob("%04d.*" % asset))
    if len(matches) != 1:
        return None
    data = matches[0].read_bytes()
    events = []
    for off in range(0, len(data) - 5, 6):
        frame, op, param = struct.unpack_from("<HHH", data, off)
        if frame == TERMINATOR and op == TERMINATOR and param == TERMINATOR:
            break
        # A track longer than the file, or a wildly out-of-range opcode, means
        # the asset is not a track at all - report nothing rather than noise.
        if op > 40:
            return None
        events.append({"frame": frame, "opcode": op,
                       "name": OPCODES.get(op, "op_%d" % op), "param": param})
    return events


def main() -> int:
    image = IMAGE.read_bytes()
    catalog = {e["asset_id"]: e
               for e in json.loads(CATALOG.read_text(encoding="utf-8"))["entries"]}

    def name(asset):
        entry = catalog.get(asset)
        if entry is None:
            return ""
        names = [n for n in (entry.get("names") or []) if n not in SKIP_NAMES]
        return names[0] if names else ""

    sets = json.loads(SETS.read_text(encoding="utf-8"))["sets"]
    sound_rows = {e["id"]: e
                  for e in json.loads(
                      SOUND_EVENTS.read_text(encoding="utf-8"))["events"]}

    tracks: dict[str, dict] = {}
    by_anim: dict[int, int] = {}
    scanned = 0
    for entry in sets:
        for record in entry.get("descriptors", []):
            events_asset = int(record.get("events", 0xFFFF))
            anim = int(record.get("anim", 0xFFFF))
            if events_asset == 0xFFFF or anim == 0xFFFF:
                continue
            if str(events_asset) in tracks:
                by_anim.setdefault(anim, events_asset)
                continue
            scanned += 1
            decoded = read_track(events_asset)
            if decoded is None:
                continue
            tracks[str(events_asset)] = {
                "asset": events_asset,
                "events": decoded,
            }
            by_anim.setdefault(anim, events_asset)

    # THE SET WALK MISSES MOST OF THE CHARACTER CLIPS.
    #
    # Walking the 630 descriptor sets finds only the arrays those sets point at,
    # and the BLASTER family's array - the one holding BlstrIdleGun,
    # BlstrWalkGun, BlstrRunGun, BlstrShootGun and the three melee swings - is
    # NOT among them. Measured: of the 163 clips the ten hunters actually play,
    # the set walk matched ZERO. Every sound the port was supposed to gain from
    # this table was therefore still silent.
    #
    # So the array region is swept directly for valid 10-byte records instead of
    # relying on set ownership. A record counts only when its `anim` is a real
    # nsbca in the catalog, its `events` is a real bin, and its state is a
    # plausible one - three independent conditions, which is what keeps a sweep
    # of raw bytes from inventing pairs.
    region_pairs = 0
    for offset in range(REGION_LO - BASE, REGION_HI - BASE - 10, 2):
        state, anim, _attached, events_asset, _queued = struct.unpack_from(
            "<HHHHH", image, offset)
        if state >= 200 or anim == 0xFFFF or events_asset == 0xFFFF:
            continue
        if (catalog.get(anim) or {}).get("extension") != "nsbca":
            continue
        if (catalog.get(events_asset) or {}).get("extension") != "bin":
            continue
        if anim in by_anim:
            continue
        key = str(events_asset)
        if key not in tracks:
            decoded = read_track(events_asset)
            if decoded is None:
                continue
            tracks[key] = {"asset": events_asset, "events": decoded,
                           "note": "found by the array-region sweep"}
        by_anim[anim] = events_asset
        region_pairs += 1

    # THE FORMAT-VERIFICATION CASE IS DECODED DIRECTLY.
    #
    # BlstrShootGun's companion is asset 529, and it is reached from a 10-byte
    # descriptor array at 0x020E9790 that NO descriptor set owns - the set table
    # has 630 entries and that array is not among their ranges. It is still real
    # data, hand-read out of the image, so it is decoded here explicitly rather
    # than dropped: it is the case that pins the record stride and field order.
    for verify in (529,):
        if str(verify) not in tracks:
            decoded = read_track(verify)
            if decoded is not None:
                tracks[str(verify)] = {"asset": verify, "events": decoded,
                                       "note": "reached from the unowned array "
                                               "at 0x020E9790"}

    # Which sounds does animation data actually raise?
    sounds: dict[int, list[int]] = {}
    fires = 0
    for key, track in tracks.items():
        for e in track["events"]:
            if e["opcode"] in SOUND_OPCODES and e["param"] < NO_SOUND:
                sounds.setdefault(e["param"], []).append(int(key))
            if e["opcode"] == 24:
                fires += 1

    # --- invariants -------------------------------------------------------
    #
    # BlstrShootGun's companion is asset 529 and its whole track is one record:
    # frame 1, opcode 24 (fire), param 5 (muzzle 5). If the record stride or the
    # field order were wrong this would not hold.
    shoot = tracks.get("529")
    if shoot is None or len(shoot["events"]) != 1:
        raise SystemExit("asset 529 must decode to exactly one event, got %s"
                         % (shoot and len(shoot["events"])))
    only = shoot["events"][0]
    if only["opcode"] != 24 or only["param"] != 5 or only["frame"] != 1:
        raise SystemExit("asset 529 must be frame 1, opcode 24, param 5 - got %s"
                         % only)

    document = {
        "schema": 1,
        "source": {
            "dispatcher": "0x01FFAD3C (ITCM)",
            "handler": "0x0209FA28",
            "record": {"+0x00": "u16 frame", "+0x02": "u16 opcode",
                       "+0x04": "u16 param"},
            "stride": 6,
            "terminator": "FFFF FFFF FFFF",
            "no_sound_sentinel": "0x%02X" % NO_SOUND,
            "opcodes": {str(k): v for k, v in sorted(OPCODES.items())},
            "sound_opcodes": sorted(SOUND_OPCODES),
            "fire_opcode": 24,
            "fire_note": "opcode 24 calls FUN_02005854 with the action row's "
                         "+0x04 kind and passes param as the MUZZLE SELECTOR",
        },
        "note": __doc__.strip(),
        "tracks": tracks,
        "anim_to_track": {str(k): v for k, v in sorted(by_anim.items())},
        # CLIP NAME -> TRACK, which is what the animator can actually look up.
        # The exported clip is named after its NSBCA asset, truncated to the
        # 16 characters the container stores, so the catalog name IS the clip
        # name the AnimationLibrary holds.
        "clip_to_track": {
            name(anim): track for anim, track in sorted(by_anim.items())
            if name(anim)
        },
        "sounds_raised": {
            str(sid): {
                "tracks": sorted(set(t))[:12],
                "wav": (sound_rows.get(sid, {}).get("variants") or [{}])[0]
                       .get("wav", ""),
            }
            for sid, t in sorted(sounds.items())
        },
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("descriptor records with an events companion: %d" % scanned)
    print("extra anim->events pairs from the region sweep:  %d" % region_pairs)
    print("tracks decoded:                              %d" % len(tracks))
    print("distinct sound ids raised by animation:      %d" % len(sounds))
    print("fire events (opcode 24):                     %d" % fires)
    print()
    print("asset 529 (BlstrShootGun): %s" % only)
    print()
    print("the sounds animation data raises:")
    for sid in sorted(sounds):
        row = sound_rows.get(sid, {})
        wav = (row.get("variants") or [{}])[0].get("wav", "")
        print("   id 0x%02X (%3d)  %-3d track(s)  %s"
              % (sid, sid, len(set(sounds[sid])), wav))
    print("\nwrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
