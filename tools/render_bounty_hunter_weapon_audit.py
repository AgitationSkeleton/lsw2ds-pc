#!/usr/bin/env python3
"""Render the ten Bounty Hunter arena characters with corroborated weapons."""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "out" / "work" / "bounty_hunter_weapon_audit"
ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
COMPONENTS = ROOT / "out" / "exported" / "runtime_character_components"
WEAPONS = ROOT / "out" / "exported" / "bounty_hunter_weapons"
CORE_BODIES = ROOT / "out" / "exported" / "playable_core_bodies"


def one_entry(asset_id: int) -> Path:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise ValueError(f"asset {asset_id}: expected one archive entry, found {len(matches)}")
    return matches[0]


def one_glb(directory: Path) -> Path:
    matches = list(directory.glob("*.glb"))
    if len(matches) != 1:
        raise ValueError(f"{directory}: expected one GLB, found {len(matches)}")
    return matches[0]


def component_texture(runtime_id: int, part: dict[str, object]) -> Path | None:
    texture = part.get("texture")
    if texture is None:
        return None
    return COMPONENTS / f"runtime_{runtime_id:03d}" / texture["png"]


def prepare_human_body(key: str, source_row: dict[str, object], animation_asset: int) -> Path:
    target = OUTPUT / "bodies" / key
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"
    subprocess.run(
        [
            str(apicula), "convert", str(one_entry(2836)), str(one_entry(2837)),
            str(one_entry(animation_asset)), "-o", str(target), "-f", "glb",
            "--more-textures", "--overwrite", "--all-animations",
        ],
        cwd=ROOT,
        check=True,
    )
    source_dir = Path(source_row["body"]).parent
    for texture in ("HeadGeneric.png", "LegsGeneric.png", "TorsoGeneric.png"):
        shutil.copyfile(source_dir / texture, target / texture)
    return target / "Human_Model.glb"


def prepare_animated_model(key: str, model_asset: int, animation_asset: int) -> Path:
    target = OUTPUT / "attachments" / key
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"
    subprocess.run(
        [
            str(apicula), "convert", str(one_entry(model_asset)), str(one_entry(animation_asset)),
            "-o", str(target), "-f", "glb", "--overwrite", "--all-animations",
        ],
        cwd=ROOT,
        check=True,
    )
    return one_glb(target)


def add_component_arguments(
    command: list[str],
    runtime_id: int,
    row: dict[str, object],
    model_root: Path,
    model_overrides: dict[int, Path] | None = None,
) -> None:
    model_overrides = model_overrides or {}
    for part in row["parts"]:
        mode = part["mode"]
        texture = component_texture(runtime_id, part)
        model = part.get("model")
        if texture is not None and mode in (0, 1, 2):
            command.append(f"--body-{part['slot']}-texture={texture}")
        if model is not None and mode in (0, 3, 4):
            slot = {0: "head", 3: "gear", 4: "back-gear"}[mode]
            model_directory = model_root / f"asset_{model['asset']:04d}"
            model_path = model_overrides.get(model["asset"], one_glb(model_directory))
            command.append(f"--{slot}={model_path}")
            if texture is not None:
                command.append(f"--{slot}-texture={texture}")


def main() -> int:
    config = json.loads((ROOT / "config" / "bounty_hunter_weapons.json").read_text(encoding="utf-8"))
    characters = config["characters"]
    human_manifest = json.loads(
        (ROOT / "out" / "work" / "runtime_character_candidates" / "render_manifest.json").read_text(
            encoding="utf-8"
        )
    )
    component_manifest = json.loads((COMPONENTS / "manifest.json").read_text(encoding="utf-8"))
    human_rows = {row["runtime_id"]: row for row in human_manifest["rows"]}
    component_rows = {row["runtime_id"]: row for row in component_manifest["runtime_appearances"]}
    model_root = COMPONENTS / "models"

    subprocess.run([sys.executable, "tools/export_bounty_hunter_weapons.py"], cwd=ROOT, check=True)
    (OUTPUT / "renders").mkdir(parents=True, exist_ok=True)

    godot = ROOT / "tools" / "vendor" / "godot" / "Godot_v4.7.1-stable_win64.exe"
    project = ROOT / "tools" / "godot_runtime_composite_preview"
    appdata = ROOT / "out" / "work" / "godot_appdata"
    appdata.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["APPDATA"] = str(appdata)
    environment["LOCALAPPDATA"] = str(appdata)
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    rendered: list[tuple[str, dict[str, object], Path]] = []
    audit_rows: list[dict[str, object]] = []
    total = len(characters)
    for index, (key, character) in enumerate(characters.items(), 1):
        runtime_id = character["runtime_id"]
        if key == "ig88":
            body = CORE_BODIES / "ig88" / "IG88_Model.glb"
        else:
            body = prepare_human_body(key, human_rows[runtime_id], character["idle_animation"])
        source = component_rows[runtime_id]
        output = OUTPUT / "renders" / f"{key}.png"
        command = [
            str(godot), "--display-driver", "windows", "--rendering-driver", "opengl3",
            "--audio-driver", "Dummy", "--path", str(project),
            "--log-file", str(OUTPUT / "godot.log"), "bounty_preview.tscn", "--",
            f"--body={body}", f"--output={output}",
            f"--animation={character['idle_name']}",
        ]
        model_overrides: dict[int, Path] = {}
        if key == "zuckuss":
            model_overrides[1221] = prepare_animated_model("zuckuss_cape", 1221, 1213)
            command.append("--back-gear-root=1")
        add_component_arguments(command, runtime_id, source, model_root, model_overrides)
        weapon = one_glb(WEAPONS / f"asset_{character['weapon_asset']:04d}")
        for hand in character["hands"]:
            command.append(f"--weapon-{hand}={weapon}")
            if character.get("weapon_roll_degrees"):
                command.append(f"--weapon-roll-{hand}={character['weapon_roll_degrees']}")
        completed = subprocess.run(
            command, cwd=ROOT, env=environment, text=True, capture_output=True,
            creationflags=creationflags,
        )
        if completed.returncode != 0:
            print(completed.stdout)
            print(completed.stderr)
            raise RuntimeError(f"Godot failed for {key} ({completed.returncode})")
        rendered.append((key, character, output))
        audit_rows.append(
            {
                "key": key,
                "label": character["label"],
                "runtime_id": runtime_id,
                "weapon_asset": character["weapon_asset"],
                "hands": character["hands"],
                "idle_animation": character["idle_animation"],
                "idle_name": character["idle_name"],
                "weapon_roll_degrees": character.get("weapon_roll_degrees", 0),
                "animated_back_gear": key == "zuckuss",
                "evidence": character["evidence"],
                "output": str(output.relative_to(ROOT)),
            }
        )
        print(f"[{index:02d}/{total:02d}] {character['label']} ({character['idle_name']})", flush=True)

    columns = 4
    tile = (320, 300)
    label_height = 38
    rows = math.ceil(len(rendered) / columns)
    sheet = Image.new("RGB", (columns * tile[0], rows * tile[1]), (12, 16, 25))
    draw = ImageDraw.Draw(sheet)
    for index, (key, character, path) in enumerate(rendered):
        x = index % columns * tile[0]
        y = index // columns * tile[1]
        image = Image.open(path).convert("RGB")
        image.thumbnail((tile[0], tile[1] - label_height), Image.Resampling.LANCZOS)
        sheet.paste(
            image,
            (x + (tile[0] - image.width) // 2,
             y + label_height + (tile[1] - label_height - image.height) // 2),
        )
        label = f"{character['label']}  |  {character['idle_name']}"
        draw.rectangle((x, y, x + tile[0] - 1, y + label_height - 1), fill=(28, 36, 54))
        draw.text((x + 8, y + 11), label, fill=(235, 240, 250))
        draw.rectangle((x, y, x + tile[0] - 1, y + tile[1] - 1), outline=(63, 75, 99))
    sheet.save(OUTPUT / "contact_sheet.png")
    (OUTPUT / "audit_manifest.json").write_text(
        json.dumps({"rows": audit_rows}, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Wrote {len(rendered)}-character Bounty Hunter audit -> {OUTPUT / 'contact_sheet.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
