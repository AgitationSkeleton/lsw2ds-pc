#!/usr/bin/env python3
"""Build the ignored Godot roster/combat catalog from ROM-derived audits.

The output contains paths to local generated assets and cannot replace the ROM.
Visual rows remain candidates until listed in config/render_approval.json.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
EXCLUDED = {19, 30, 45, 49, 57}
SPECIAL = {
    1: "Anakin Skywalker (Force Ghost)", 9: "C-3PO", 10: "Chewbacca",
    16: "Ewok", 17: "Gamorrean Guard", 23: "Han Solo (in Carbonite)",
    27: "IG-88", 31: "Jawa", 32: "K-3PO", 50: "MSE-6",
    52: "Obi-Wan Kenobi (Force Ghost)", 53: "R2-D2", 54: "R5-D4",
    55: "Gonk Droid", 59: "Salacious Crumb", 67: "Wicket", 69: "Yoda",
    70: "Yoda (Force Ghost)",
}
KNOWN = {
    0: "4-LOM", 2: "Biggs Darklighter", 3: "Bith 1", 4: "Bith 2",
    5: "Bith 3", 6: "Bith 4", 7: "Boba Fett", 8: "Bossk",
    11: "Darth Vader", 12: "Death Star Trooper", 13: "Dengar",
    14: "Emperor Palpatine", 15: "Emperor's Guard", 18: "Custom Character 1",
    20: "Greedo", 21: "Han Solo (variant 1)", 22: "Han Solo (variant 2)",
    24: "Han Solo (variant 3)", 25: "Han Solo (variant 4)",
    26: "Han Solo (Stormtrooper)", 28: "Imperial Officer", 29: "Jango Fett",
    33: "Lando Calrissian", 34: "Lando Calrissian (Jabba's Palace)",
    35: "Princess Leia", 36: "Princess Leia (Boushh)",
    37: "Princess Leia (Cloud City)", 38: "Princess Leia (Endor)",
    39: "Princess Leia (Hoth)", 40: "Princess Leia (Jabba's Prisoner)",
    41: "Luke Skywalker (variant 1)", 42: "Luke Skywalker (variant 2)",
    43: "Luke Skywalker (variant 3)", 44: "Luke Skywalker (Endor)",
    46: "Luke Skywalker (Jabba's Palace)", 47: "Luke Skywalker (Stormtrooper)",
    48: "Mace Windu", 51: "Obi-Wan Kenobi", 56: "Rebel Trooper",
    58: "Rebel Trooper (Hoth)", 60: "Sandtrooper", 61: "Scout Trooper",
    62: "Shuttle Pilot", 63: "Snowtrooper", 64: "Stormtrooper",
    65: "TIE Fighter Pilot", 66: "Tusken Raider", 68: "Wuher", 71: "Zuckuss",
    **SPECIAL,
}
BOUNTY = {0, 7, 8, 13, 20, 27, 29, 34, 36, 71}


def first_glb(folder: Path) -> Path | None:
    matches = sorted(folder.glob("*.glb"))
    return matches[0] if matches else None


def relative(path: Path | None) -> str:
    return "" if path is None else path.resolve().relative_to(ROOT.resolve()).as_posix()


def semantic_asset_name(catalog: list[dict], asset_id: int) -> str:
    if not 0 <= asset_id < len(catalog):
        return ""
    boilerplate = {"BCA0", "JNT0", "BMD0", "MDL0", "TEX0"}
    return next(
        (str(name) for name in catalog[asset_id].get("names", []) if str(name) not in boilerplate),
        "",
    )


def annotate_animation_maps(selected: dict, catalog: list[dict]) -> dict:
    result = {}
    for role, decoded in selected.get("decoded_animation_maps", {}).items():
        if not isinstance(decoded, dict) or decoded.get("status") != "decoded":
            result[role] = decoded
            continue
        copy = dict(decoded)
        copy["records"] = []
        for source in decoded["records"]:
            record = dict(source)
            record["animation_names"] = [
                semantic_asset_name(catalog, int(asset_id))
                for asset_id in source["animation_assets"]
            ]
            copy["records"].append(record)
        result[role] = copy
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/exported/godot_runtime/roster.json")
    args = parser.parse_args()
    actions = json.loads((ROOT / "out/work/actor_action_rows.json").read_text(encoding="utf-8"))
    archive_catalog = json.loads(
        (ROOT / "out/raw/archive/catalog.json").read_text(encoding="utf-8")
    )["entries"]
    descriptors = {row["runtime_id"]: row for row in actions["playable_runtime_descriptors"]}
    weapon_config = json.loads((ROOT / "config/bounty_hunter_weapons.json").read_text(encoding="utf-8"))
    weapons = {int(row["runtime_id"]): row for row in weapon_config["characters"].values()}

    # EVERY WEAPON IS CHECKED AGAINST THE ROM BEFORE IT IS EXPORTED.
    #
    # bounty_hunter_weapons.json was assembled by matching MiniKit and Statue
    # part names against standalone models. That got nine hunters right and
    # 4-LOM wrong: his MiniKit does name BobaRifle, and the cartridge hands him
    # a StTrprBlstr. config/weapon_models.json reads the real chain instead -
    # action row +0x00 is a weapon id, and 0x020EB7BC + 8*id is the model - so
    # any disagreement between the two is a defect in the hand-assembled file
    # and the export stops rather than baking it into the game.
    rom_weapons_path = ROOT / "config/weapon_models.json"
    if rom_weapons_path.exists():
        rom_weapons = {
            int(row["runtime_id"]): row
            for row in json.loads(
                rom_weapons_path.read_text(encoding="utf-8"))["characters"]
        }
        disagree = []
        for runtime_id, row in sorted(weapons.items()):
            rom_row = rom_weapons.get(runtime_id)
            if rom_row is None or rom_row.get("model_asset") is None:
                continue
            if int(row["weapon_asset"]) != int(rom_row["model_asset"]):
                disagree.append(
                    "  %-24s config says %s, the ROM says %s (%s)"
                    % (row.get("label", runtime_id), row["weapon_asset"],
                       rom_row["model_asset"], rom_row["model_name"]))
        if disagree:
            raise SystemExit(
                "config/bounty_hunter_weapons.json disagrees with the ROM:\n"
                + "\n".join(disagree)
                + "\nfix the config, or re-run tools/export_weapon_models.py "
                  "if the ROM read has changed")
    components_path = ROOT / "out/exported/runtime_character_components/manifest.json"
    component_rows = {}
    if components_path.exists():
        component_manifest = json.loads(components_path.read_text(encoding="utf-8"))
        component_rows = {row["runtime_id"]: row for row in component_manifest["runtime_appearances"]}
    humanoid_manifest_path = ROOT / "out/work/runtime_character_candidates/render_manifest.json"
    humanoids = {}
    if humanoid_manifest_path.exists():
        manifest = json.loads(humanoid_manifest_path.read_text(encoding="utf-8"))
        humanoids = {row["runtime_id"]: row for row in manifest["rows"]}
    rows = []
    for runtime_id in range(72):
        if runtime_id in EXCLUDED:
            continue
        descriptor = descriptors[runtime_id]
        selected = descriptor["selected_action"]
        primary = selected["resolved_primary"]
        body = None
        assembly = "missing"
        attachments = {}
        if runtime_id in humanoids:
            source = humanoids[runtime_id]
            body = Path(source["body"])
            for key in ("head", "gear", "back_gear"):
                if not source.get(key):
                    continue
                slot = "back_gear" if key == "back_gear" else key
                part = next((part for part in component_rows.get(runtime_id, {}).get("parts", []) if part["slot"] == slot), {})
                texture = part.get("texture", {}).get("png")
                attachments[key] = {
                    "path": relative(Path(source[key])),
                    "texture": relative(ROOT / f"out/exported/runtime_character_components/runtime_{runtime_id:03d}" / texture) if texture else "",
                    "bone": "Lego_Chest" if key == "back_gear" else "Lego_Head",
                }
                if key == "back_gear":
                    # Back models come in two authoring spaces and the runtime
                    # has to know which. Capes carry their own torso height and
                    # must sit at identity; attaching one to the chest applies
                    # that height twice and drapes it over the character's face.
                    # Jetpacks are built around their own origin and need the
                    # bone or they land at the feet. The measurement is made
                    # when the candidate manifest is built; it was being dropped
                    # here, so the game got it wrong while the renders got it
                    # right.
                    space = source.get("back_gear_space", "origin")
                    attachments[key]["space"] = space
                    if space == "character":
                        attachments[key]["bone"] = ""
            assembly = "humanoid_candidate"
        elif runtime_id in SPECIAL:
            body = first_glb(ROOT / f"out/work/runtime_special_candidates/runtime_{runtime_id:03d}")
            assembly = "special_body_candidate"
        weapon = weapons.get(runtime_id)
        weapon_profile = {}
        if weapon is not None:
            weapon_asset = int(weapon["weapon_asset"])
            weapon_profile = {
                "asset": weapon_asset,
                "path": relative(first_glb(ROOT / f"out/exported/bounty_hunter_weapons/asset_{weapon_asset:04d}")),
                "hands": weapon["hands"],
                "roll_degrees": float(weapon.get("weapon_roll_degrees", 0.0)),
                "secondary_asset": weapon.get("secondary_asset"),
            }
        rows.append({
            "runtime_id": runtime_id,
            "label": KNOWN.get(runtime_id, f"Runtime {runtime_id}"),
            "name_status": "resolved" if "variant" not in KNOWN.get(runtime_id, "") else "family_only",
            "bounty_hunter": runtime_id in BOUNTY,
            "visual_status": assembly,
            "body": relative(body),
            "attachments": attachments,
            "action_row": descriptor["action_row_u8_05"],
            "dispatch_mode": selected["dispatch_mode_u8_03"],
            "selection_resource": selected["selection_resource_u16_00"],
            "primary_action": selected["primary_action_u8_04"],
            "action_flags": selected["flags_u16_18"],
            "auxiliary_emitter_selector": selected["auxiliary_emitter_selector_u8_1a"],
            "animation_map_primary": selected["animation_map_0c"],
            "animation_map_auxiliary": selected["animation_map_10"],
            "animation_maps": annotate_animation_maps(selected, archive_catalog),
            "cooldown_updates": selected["cooldown_u8_18"],
            "damage_type": primary.get("damage_type"),
            "damage": primary.get("damage"),
            "projectile_gravity": primary.get("gravity_enabled", False),
            "action_kind": primary.get("kind", "unresolved"),
            "visible_weapon": weapon_profile,
        })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "schema_version": 1,
        "rom_required": True,
        "playable_count": len(rows),
        "excluded_runtime_ids": sorted(EXCLUDED),
        "rows": rows,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {len(rows)} playable runtime profiles -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
