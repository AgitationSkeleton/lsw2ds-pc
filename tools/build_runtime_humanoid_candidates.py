#!/usr/bin/env python3
"""Assemble standard-humanoid runtime rows into Godot render candidates."""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
from pathlib import Path

from export_runtime_character_components import decode_companion


ROOT = Path(__file__).resolve().parent.parent
STANDARD_HUMANOID_ROWS = (
    0, 2, 3, 4, 5, 6, 7, 8,
    11, 12, 13, 14, 15,
    20, 21, 22, 24, 25, 26, 28, 29,
    33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48,
    51, 56, 58, 60, 61, 62, 63, 64, 65, 66, 68, 71,
)

KNOWN_NAMES = {
    0: "4-LOM", 2: "Biggs Darklighter", 3: "Bith 1", 4: "Bith 2",
    5: "Bith 3", 6: "Bith 4", 7: "Boba Fett", 8: "Bossk",
    11: "Darth Vader", 12: "Death Star Trooper", 13: "Dengar",
    14: "Emperor Palpatine", 15: "Emperor's Guard", 20: "Greedo",
    27: "IG-88", 28: "Imperial Officer", 29: "Jango Fett",
    31: "Jawa", 32: "K-3PO", 48: "Mace Windu", 51: "Obi-Wan Kenobi",
    52: "Obi-Wan (Force Ghost)", 53: "R2-D2", 54: "R5-D4",
    56: "Rebel Trooper", 58: "Rebel Trooper (Hoth)",
    60: "Sandtrooper", 61: "Scout Trooper", 62: "Shuttle Pilot",
    63: "Snowtrooper", 64: "Stormtrooper", 65: "TIE Fighter Pilot",
    66: "Tusken Raider", 68: "Wuher", 69: "Yoda",
    70: "Yoda (Force Ghost)", 71: "Zuckuss",
}

BODY_TEXTURES = {0: "HeadGeneric.png", 1: "LegsGeneric.png", 2: "TorsoGeneric.png"}
SLOT_KEYS = {0: "head", 3: "gear", 4: "back_gear"}


# Back gear is authored two ways and the difference is measurable from the GLB.
# Capes carry their own torso height - Cape_Model's geometry spans y 0.68..6.29 -
# so they are already in complete-character space and must be added at identity.
# Packs are built around their own origin - BobaFettPack spans -2.25..-0.94 - so
# at identity they land under the feet and need the chest bone. Deciding this per
# asset is what stopped jetpacks rendering at the character's feet.
SELF_POSITIONED_MIN_Y = 1.5


def glb_document(path: Path) -> dict:
    data = path.read_bytes()
    offset = 12
    while offset < len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + length]
        offset += length
        if kind == 0x4E4F534A:
            return json.loads(chunk.decode("utf-8"))
    return {}


def model_centre_y(path: Path) -> float | None:
    document = glb_document(path)
    nodes = document.get("nodes", [])
    parent: dict[int, int] = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parent[child] = index

    def folded(index: int) -> tuple[list[float], list[float]]:
        translation = [0.0, 0.0, 0.0]
        scale = [1.0, 1.0, 1.0]
        stack: list[int] = []
        cursor: int | None = index
        while cursor is not None:
            stack.append(cursor)
            cursor = parent.get(cursor)
        for step in reversed(stack):
            node = nodes[step]
            node_scale = node.get("scale", [1, 1, 1])
            node_translation = node.get("translation", [0, 0, 0])
            for axis in range(3):
                translation[axis] += scale[axis] * node_translation[axis]
                scale[axis] *= node_scale[axis]
        return translation, scale

    low, high = None, None
    for index, node in enumerate(nodes):
        if "mesh" not in node:
            continue
        translation, scale = folded(index)
        for primitive in document["meshes"][node["mesh"]]["primitives"]:
            accessor = document["accessors"][primitive["attributes"]["POSITION"]]
            if "min" not in accessor:
                continue
            bottom = accessor["min"][1] * scale[1] + translation[1]
            top = accessor["max"][1] * scale[1] + translation[1]
            low = bottom if low is None else min(low, bottom)
            high = top if high is None else max(high, top)
    return None if low is None else (low + high) / 2.0


def back_gear_space(path: Path) -> str:
    centre = model_centre_y(path)
    return "character" if centre is not None and centre > SELF_POSITIONED_MIN_Y else "origin"


def one_entry(entries: Path, asset_id: int) -> Path:
    matches = list(entries.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise ValueError(f"asset {asset_id}: expected one archive entry, found {len(matches)}")
    return matches[0]


def first_glb(model_root: Path, asset_id: int) -> Path:
    matches = list((model_root / f"asset_{asset_id:04d}").glob("*.glb"))
    if len(matches) != 1:
        raise ValueError(f"model asset {asset_id}: expected one GLB, found {len(matches)}")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--components",
        type=Path,
        default=Path("out/exported/runtime_character_components/manifest.json"),
    )
    parser.add_argument("--entries", type=Path, default=Path("out/raw/archive/entries"))
    parser.add_argument("--output", type=Path, default=Path("out/work/runtime_character_candidates"))
    parser.add_argument("--rows", nargs="*", type=int, default=list(STANDARD_HUMANOID_ROWS))
    parser.add_argument("--animation", type=int, default=491)
    args = parser.parse_args()

    manifest_path = (ROOT / args.components).resolve()
    entries = (ROOT / args.entries).resolve()
    output = (ROOT / args.output).resolve()
    model_root = manifest_path.parent / "models"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    rows = {row["runtime_id"]: row for row in manifest["runtime_appearances"]}
    output.mkdir(parents=True, exist_ok=True)

    template = output / "_human_template"
    template.mkdir(parents=True, exist_ok=True)
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"
    subprocess.run(
        [
            str(apicula), "convert", str(one_entry(entries, 2836)), str(one_entry(entries, 2837)),
            str(one_entry(entries, args.animation)), "-o", str(template), "-f", "glb",
            "--more-textures", "--overwrite", "--all-animations",
        ],
        cwd=ROOT,
        check=True,
    )

    render_rows: list[dict[str, object]] = []
    for runtime_id in args.rows:
        row = rows[runtime_id]
        row_dir = output / f"runtime_{runtime_id:03d}"
        if row_dir.exists():
            shutil.rmtree(row_dir)
        shutil.copytree(template, row_dir)
        render: dict[str, object] = {
            "runtime_id": runtime_id,
            "label": KNOWN_NAMES.get(runtime_id, f"runtime {runtime_id:02d}"),
            "body": str((row_dir / "Human_Model.glb").resolve()),
            "output": str((output / "renders" / f"runtime_{runtime_id:03d}.png").resolve()),
            "animation": "BlstrIdle",
            "parts": [],
        }
        for part in row["parts"]:
            mode = part["mode"]
            companion = part["companion_asset"]
            texture_path: Path | None = None
            if companion is not None:
                texture_path = row_dir / "runtime_textures" / f"mode_{mode}_asset_{companion:04d}.png"
                decode_companion(one_entry(entries, companion), texture_path)
            model = part.get("model")
            if mode in BODY_TEXTURES and texture_path is not None:
                shutil.copyfile(texture_path, row_dir / BODY_TEXTURES[mode])
            if mode in SLOT_KEYS and model is not None:
                model_asset = model["asset"]
                slot_path = first_glb(model_root, model_asset).resolve()
                render[SLOT_KEYS[mode]] = str(slot_path)
                if texture_path is not None:
                    render[f"{SLOT_KEYS[mode]}_texture"] = str(texture_path.resolve())
                if SLOT_KEYS[mode] == "back_gear":
                    render["back_gear_space"] = back_gear_space(slot_path)
            render["parts"].append(
                {
                    "actor_part": part["actor_part"],
                    "mode": mode,
                    "companion_asset": companion,
                    "model_asset": model["asset"] if model is not None else None,
                }
            )
        render_rows.append(render)
        print(f"Prepared runtime {runtime_id:03d}: {render['label']}")

    render_manifest = output / "render_manifest.json"
    render_manifest.write_text(json.dumps({"rows": render_rows}, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(render_rows)} candidates -> {render_manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
