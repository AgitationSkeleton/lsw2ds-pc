#!/usr/bin/env python3
"""Convert every decoded character animation into Godot-loadable GLB clips.

The runtime rows already carry decoded animation-state maps, but the render
pipeline only ever converted one clip per character - the idle - so the port
had nothing to locomote with. This converts the whole set.

Standard humanoids share body assets 2836/2837, so their animations go into a
single shared library rather than being duplicated 49 times. Each character
then selects clips out of it by name using the state map in roster.json.

Death is handled separately on purpose: no decoded animation map references any
death clip, so the game must pick one by body type elsewhere. The death clips
below are matched by archive name, which is a correlation and not a decoded
binding.
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
DEFAULT_ROSTER = ROOT / "out" / "exported" / "godot_runtime" / "roster.json"
DEFAULT_OUTPUT = ROOT / "out" / "exported" / "godot_runtime" / "animations"
APICULA = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"

HUMANOID_BODY = (2836, 2837)

# Death is a SEPARATE MODEL, not a clip on the normal skeleton. The LEGO figure
# breaks apart, so the sequence needs its own geometry plus a visibility track
# that hides the head. This is why the death clips fail to bind to Human_Model:
# they target a different object set entirely.
#
#   HumanDeath_Model  2829  +  HumanDeath      2827  +  HumanDeathNoHeadV  2828
#   HumanSmallDeath_  2834  +  HumanSmallDeath 2832  +  HumanSmallDeathNV  2833
#
# Which body type gets which is a name correlation; no decoded map references
# either, so the selection path itself is still untraced.
DEATH_SETS = {
    "human": {"model": 2829, "animation": 2827, "visibility": 2828},
    "human_small": {"model": 2834, "animation": 2832, "visibility": 2833},
    # Ewok and Wicket follow the same break-apart pattern: a dedicated death
    # model beside the ordinary one. Ewok also has the matching clip; Wicket
    # ships only the model.
    "ewok": {"model": 2193, "animation": 2192, "visibility": 0},
    "wicket": {"model": 5213, "animation": 0, "visibility": 0},
}
DEATH_CLIPS = {name: data["animation"] for name, data in DEATH_SETS.items()}

# Characters on their own geometry cannot use the shared humanoid library: its
# clips target Human_Model's objects. Each of these bodies gets its own library,
# built from the archive clips whose names carry the body's prefix. The prefix
# match is a name correlation, but an exact one - every clip listed under a
# prefix belongs unambiguously to that character.
SPECIAL_BODIES = {
    "c3po": {"model": 933, "prefix": "C3PO", "runtime_ids": [9, 32]},
    "r2d2_r5d4": {"model": 4012, "prefix": "R2D2", "runtime_ids": [53, 54]},
    "mse6": {"model": 3789, "prefix": "MSE6", "runtime_ids": [50]},
    "gonk": {"model": 3923, "prefix": "PowerDroid", "runtime_ids": [55]},
    "salacious_crumb": {"model": 4211, "prefix": "Salacious", "runtime_ids": [59]},
    "han_in_carbonite": {"model": 2551, "prefix": "HanCarbonite", "runtime_ids": [23]},
    "chewbacca": {"model": 1472, "prefix": "Chewbacca", "runtime_ids": [10]},
    "ig88": {"model": 2935, "prefix": "IG88", "runtime_ids": [27]},
    "gamorrean_guard": {"model": 2416, "prefix": "GamGuard", "runtime_ids": [17]},
    "ewok": {"model": 2191, "prefix": "Ewok", "runtime_ids": [16]},
    "wicket": {"model": 5212, "prefix": "Wicket", "runtime_ids": [67]},
}


def load_magic(entries: Path) -> dict[int, str]:
    catalog = json.loads(
        (entries.parent / "catalog.json").read_text(encoding="utf-8")
    )
    return {e["asset_id"]: e.get("magic", "") for e in catalog["entries"]}


def entry(entries: Path, asset_id: int) -> Path:
    matches = list(entries.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise RuntimeError(f"asset {asset_id}: expected one entry, found {matches}")
    return matches[0]


def decoded_maps(row: dict) -> list[dict]:
    maps = row.get("animation_maps") or {}
    return [
        maps[key] for key in ("primary", "auxiliary")
        if maps.get(key) and maps[key].get("status") == "decoded"
    ]


def animation_assets(row: dict, magic: dict[int, str]) -> list[int]:
    # Only the first slot of each record is a BCA0 clip; the remaining slots
    # are small auxiliary records that apicula cannot read and that are not
    # animations at all.
    found: list[int] = []
    for block in decoded_maps(row):
        for record in block.get("records", []):
            for asset in record.get("animation_assets", []):
                if asset in (0, 0xFFFF) or asset in found:
                    continue
                if magic.get(asset) != "BCA0":
                    continue
                found.append(asset)
    return found


def clips_with_prefix(entries: Path, magic: dict[int, str], prefix: str) -> list[int]:
    catalog = json.loads((entries.parent / "catalog.json").read_text(encoding="utf-8"))
    found: list[int] = []
    for record in catalog["entries"]:
        if record.get("magic") != "BCA0":
            continue
        for name in record.get("names") or []:
            # Nitro block tags lead the name list; skip them to reach the clip
            # name. JNT0 appears with trailing junk such as JNT0X, so match the
            # tag rather than the exact string.
            if not name or not name.isprintable():
                continue
            if name.startswith(("BCA0", "JNT", "VIS", "SRT")):
                continue
            if name.startswith(prefix):
                found.append(record["asset_id"])
            break
    return found


def glb_animation_names(path: Path) -> list[str]:
    data = path.read_bytes()
    offset = 12
    while offset < len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + length]
        offset += length
        if kind == 0x4E4F534A:
            document = json.loads(chunk.decode("utf-8"))
            return [a.get("name", "") for a in document.get("animations", [])]
    return []


def convert(entries: Path, output: Path, name: str, assets: list[int]) -> dict:
    target = output / name
    target.mkdir(parents=True, exist_ok=True)
    command = [
        str(APICULA), "convert",
        *(str(entry(entries, asset)) for asset in assets),
        "-o", str(target), "-f", "glb",
        "--more-textures", "--overwrite", "--all-animations",
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    glbs = sorted(target.glob("*.glb"))
    clips: list[str] = []
    for glb in glbs:
        clips.extend(glb_animation_names(glb))
    return {
        "name": name,
        "status": "exported" if completed.returncode == 0 and glbs else "failed",
        "glb": [g.relative_to(ROOT).as_posix() for g in glbs],
        "clip_count": len(clips),
        "clips": clips,
        "diagnostic": (completed.stdout + completed.stderr).strip()[-400:],
    }


# Special bodies have no decoded animation map, so their states are derived
# from clip-name suffixes. This is a NAME CORRELATION, not a decoded binding,
# and is reported as such in the manifest.
DERIVED_STATES = [
    ("armed_idle", ("IdleGun", "IdleGn")),
    ("armed_walk", ("WalkGun", "WalkGn")),
    ("armed_run", ("RunGun",)),
    ("primary_fire", ("Shoot", "Fire")),
    ("melee_1", ("Melee1", "Attack1")),
    ("melee_2", ("Melee2", "Attack2")),
    ("melee_3", ("Melee3", "Attack3")),
    ("jump_or_airborne", ("JumpUp", "Jump", "Hop")),
    ("jump_land", ("JumpLand", "Land")),
    ("fall", ("Fall",)),
    ("unarmed_walk", ("Walk",)),
    ("unarmed_run", ("Run",)),
    ("unarmed_idle", ("Idle",)),
]


# Archive names are truncated to sixteen characters and joint animations gain a
# trailing J, so SalaciousCrumbIdle becomes SalaciousCrumbIdJ. These looser
# tokens catch the truncated forms.
DERIVED_TOKENS = [
    ("primary_fire", ("Shoot", "Fire")),
    ("melee_1", ("Melee1", "Attack1")),
    ("melee_2", ("Melee2", "Attack2")),
    ("melee_3", ("Melee3", "Attack3")),
    ("jump_or_airborne", ("Jump", "Ju", "Hop")),
    ("fall", ("Fall",)),
    ("unarmed_walk", ("Walk", "Wa")),
    ("unarmed_run", ("Run",)),
    ("unarmed_idle", ("Idle", "Id")),
]


def derive_states(clips: list[str]) -> dict:
    """Map clip names onto the decoded state vocabulary by suffix."""
    states: dict[str, dict] = {}
    for state, suffixes in DERIVED_STATES:
        if state in states:
            continue
        for clip in clips:
            if clip.endswith("Death"):
                continue
            if any(clip.endswith(suffix) for suffix in suffixes):
                states[state] = {"clip": clip, "derived": True}
                break
    # Second pass for truncated names, which lose their suffix.
    for state, tokens in DERIVED_TOKENS:
        if state in states:
            continue
        for clip in clips:
            if clip.endswith("Death"):
                continue
            trimmed = clip[:-1] if clip.endswith("J") else clip
            if any(trimmed.endswith(token) for token in tokens):
                states[state] = {"clip": clip, "derived": True}
                break
    # A body with clips but no recognisable state still needs something to
    # stand on, so its first clip becomes idle.
    if not states and clips:
        first = next((c for c in clips if not c.endswith("Death")), clips[0])
        states["unarmed_idle"] = {"clip": first, "derived": True}
    # Mirror the armed family onto the unarmed one and back so selection always
    # resolves for bodies that only carry one of them.
    for pair in (("armed_idle", "unarmed_idle"), ("armed_walk", "unarmed_walk"),
                 ("armed_run", "unarmed_run")):
        if pair[0] not in states and pair[1] in states:
            states[pair[0]] = dict(states[pair[1]])
        elif pair[1] not in states and pair[0] in states:
            states[pair[1]] = dict(states[pair[0]])
    return states


def convert_batched(entries: Path, output: Path, name: str, base: list[int],
                    assets: list[int], batch_size: int = 8) -> dict:
    """Convert in batches, isolating clips the converter cannot bind.

    apicula aborts the whole run when one clip is incompatible with the target
    skeleton, so a failing batch is retried one clip at a time. That keeps the
    usable clips and names the unusable ones instead of losing everything.
    """
    library = {"name": name, "status": "exported", "glb": [], "clips": [],
               "batches": [], "rejected": []}
    index = 0
    pending = list(assets)
    while pending:
        batch, pending = pending[:batch_size], pending[batch_size:]
        result = convert(entries, output, "%s_%02d" % (name, index), [*base, *batch])
        if result["status"] == "exported":
            library["glb"].extend(result["glb"])
            library["clips"].extend(result["clips"])
            library["batches"].append({"index": index, "assets": batch,
                                       "clips": result["clip_count"],
                                       "clip_names": result["clips"],
                                       "glb": result["glb"]})
            index += 1
            continue
        for asset in batch:
            single = convert(entries, output, "%s_%02d" % (name, index), [*base, asset])
            if single["status"] == "exported":
                library["glb"].extend(single["glb"])
                library["clips"].extend(single["clips"])
                library["batches"].append({"index": index, "assets": [asset],
                                           "clips": single["clip_count"],
                                           "clip_names": single["clips"],
                                           "glb": single["glb"]})
                index += 1
            else:
                library["rejected"].append(asset)
    library["clip_count"] = len(library["clips"])
    if not library["clips"]:
        library["status"] = "failed"
    return library


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", type=Path, default=DEFAULT_ENTRIES)
    parser.add_argument("--roster", type=Path, default=DEFAULT_ROSTER)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--bounty-only", action="store_true",
        help="restrict the shared library to the ten arena characters",
    )
    args = parser.parse_args()

    magic = load_magic(args.entries)
    roster = json.loads(args.roster.read_text(encoding="utf-8"))
    rows = roster["rows"]
    if args.bounty_only:
        rows = [row for row in rows if row.get("bounty_hunter")]

    union: list[int] = []
    for row in rows:
        for asset in animation_assets(row, magic):
            if asset not in union:
                union.append(asset)

    args.output.mkdir(parents=True, exist_ok=True)
    # apicula aborts the whole conversion if any single clip is incompatible
    # with the body skeleton, so convert in batches and fall back to one clip
    # at a time for a failing batch. That isolates the bad clips instead of
    # losing the set.
    library = {"name": "humanoid", "status": "exported", "glb": [], "clips": [],
               "batches": [], "rejected": []}
    batch_size = 8
    batch_index = 0
    pending = list(union)
    while pending:
        batch = pending[:batch_size]
        pending = pending[batch_size:]
        result = convert(
            args.entries, args.output, "humanoid_%02d" % batch_index,
            [*HUMANOID_BODY, *batch],
        )
        if result["status"] == "exported":
            library["glb"].extend(result["glb"])
            library["clips"].extend(result["clips"])
            library["batches"].append({"index": batch_index, "assets": batch,
                                       "clips": result["clip_count"],
                                       "clip_names": result["clips"],
                                       "glb": result["glb"]})
            batch_index += 1
            continue
        # Retry the batch one clip at a time.
        for asset in batch:
            single = convert(
                args.entries, args.output, "humanoid_%02d" % batch_index,
                [*HUMANOID_BODY, asset],
            )
            if single["status"] == "exported":
                library["glb"].extend(single["glb"])
                library["clips"].extend(single["clips"])
                library["batches"].append({"index": batch_index, "assets": [asset],
                                           "clips": single["clip_count"],
                                           "clip_names": single["clips"],
                                           "glb": single["glb"]})
                batch_index += 1
            else:
                library["rejected"].append(asset)
    library["clip_count"] = len(library["clips"])
    if not library["clips"]:
        library["status"] = "failed"

    # Resolve asset -> clip name by conversion order. apicula emits animations
    # in the order they were passed, and it recovers proper names where the
    # roster's first-archive-name heuristic picked a meaningless one such as
    # JNT00.
    asset_to_clip: dict[int, str] = {}
    for batch in library["batches"]:
        clips = batch.get("clip_names", [])
        for index, asset in enumerate(batch["assets"]):
            if index < len(clips):
                asset_to_clip[asset] = clips[index]
    library["asset_to_clip"] = {str(k): v for k, v in sorted(asset_to_clip.items())}

    # Bodies with their own geometry get their own libraries.
    special = {}
    for body, data in SPECIAL_BODIES.items():
        assets = clips_with_prefix(args.entries, magic, str(data["prefix"]))
        if not assets:
            special[body] = {"status": "no clips", "clips": [], "glb": []}
            continue
        result = convert_batched(
            args.entries, args.output, "body_%s" % body,
            [int(data["model"])], assets,
        )
        result["runtime_ids"] = data["runtime_ids"]
        result["prefix"] = data["prefix"]
        result["derived_states"] = derive_states(result["clips"])
        result["death_clip"] = next(
            (c for c in result["clips"] if c.endswith("Death")), ""
        )
        special[body] = result

    # Death sets convert on their own geometry.
    death_groups = {}
    for name, data in DEATH_SETS.items():
        assets = [data["model"]]
        for key in ("animation", "visibility"):
            if data.get(key):
                assets.append(data[key])
        death_groups[name] = convert(
            args.entries, args.output, "death_%s" % name, assets,
        )

    # Per-character state -> clip. The clip name is the archive name recorded
    # in the roster; where the archive name is missing or meaningless the asset
    # id is kept so the binding can still be resolved by hand.
    characters = []
    for row in rows:
        states: dict[str, dict] = {}
        for block in decoded_maps(row):
            for record in block.get("records", []):
                name = record.get("state_name", "unknown")
                if name == "unknown":
                    name = "state_0x%02x" % int(record["state"])
                assets = [
                    a for a in record.get("animation_assets", [])
                    if a not in (0, 0xFFFF) and magic.get(a) == "BCA0"
                ]
                names = [n for n in (record.get("animation_names") or []) if n]
                if not assets:
                    continue
                resolved = asset_to_clip.get(assets[0], "")
                states.setdefault(name, {
                    "asset": assets[0],
                    # Prefer the name apicula recovered from the clip itself.
                    "clip": resolved or (names[0] if names else ""),
                    "state": int(record["state"]),
                })
        body_key = ""
        binding = "shared_humanoid"
        for key, data in SPECIAL_BODIES.items():
            if row["runtime_id"] in data["runtime_ids"]:
                body_key = key
                break
        decoded_states = states
        if body_key and body_key in special:
            # The decoded map names clips from the shared human rig. A
            # character on its own geometry cannot bind those - IG-88's idle
            # resolves 0% of its tracks against IG88_Model - so its own clips
            # win. Whether retail retargets the shared clips instead is not
            # known, so the decoded map is preserved alongside rather than
            # discarded.
            derived = special[body_key].get("derived_states", {})
            if derived:
                states = derived
                binding = "own_clips"
            else:
                # The body has no clips of its own, so the decoded map's shared
                # human clips are all that is left - and they will not bind to
                # this skeleton. Say so rather than let it look healthy.
                binding = "unbound"
        if not states and not body_key:
            # Rows on the shared human body with no decoded map fall back to
            # the generic Blstr locomotion set. Correlation, not decoded.
            generic = {
                "armed_idle": "BlstrIdleGun", "armed_walk": "BlstrWalkGun",
                "armed_run": "BlstrRunGun", "unarmed_idle": "BlstrIdle",
                "unarmed_walk": "BlstrWalk", "fall": "BlstrFall",
                "jump_or_airborne": "BlstrJumpUp", "jump_fall": "BlstrJumpFall",
                "jump_land": "BlstrJumpLand", "primary_fire": "BlstrShootGun",
            }
            states = {
                name: {"clip": clip, "derived": True}
                for name, clip in generic.items()
                if clip in library["clips"]
            }
        characters.append({
            "special_body": body_key,
            "animation_binding": binding,
            "decoded_states": sorted(decoded_states) if body_key else [],
            "runtime_id": row["runtime_id"],
            "label": row.get("label", ""),
            "bounty_hunter": bool(row.get("bounty_hunter")),
            "states": states,
        })

    manifest = {
        "schema_version": 1,
        "purpose": "Converted character animation clips and the per-character state map.",
        "library": library,
        "special_bodies": {
            "status": "Clips matched to a body by archive-name prefix. The match "
                      "is exact in the sense that every clip under a prefix "
                      "belongs to that character, but the binding itself is a "
                      "name correlation and not decoded.",
            "bodies": special,
        },
        "death": {
            "status": "Death uses a dedicated break-apart model plus a visibility "
                      "track, not a clip on the normal skeleton, which is why the "
                      "death animations refuse to bind to Human_Model. No decoded "
                      "animation map references them, so the selection path is "
                      "still untraced and the body-type pairing is a name "
                      "correlation.",
            "sets": DEATH_SETS,
            "exported": death_groups,
        },
        "character_count": len(characters),
        "characters": characters,
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=1) + "\n", encoding="utf-8"
    )
    print(
        f"library {library['status']}: {library['clip_count']} clips from "
        f"{len(union)} animation assets across {len(library['batches'])} GLBs "
        f"-> {args.output}"
    )
    if library["rejected"]:
        print(f"  rejected {len(library['rejected'])} incompatible clips: "
              f"{library['rejected']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
