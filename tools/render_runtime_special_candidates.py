#!/usr/bin/env python3
"""Assemble and render nonstandard-body playable runtime candidates."""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "out" / "work" / "runtime_special_candidates"
COMPONENTS = ROOT / "out" / "exported" / "runtime_character_components"
BODIES = ROOT / "out" / "exported" / "playable_core_bodies"

ROWS = {
    1: ("Anakin Skywalker (Force Ghost)", "ghost_human"),
    9: ("C-3PO", "c3po"),
    10: ("Chewbacca", "chewbacca"),
    16: ("Ewok", "ewok"),
    17: ("Gamorrean Guard", "gamorrean_guard"),
    23: ("Han Solo (in Carbonite)", "han_in_carbonite"),
    27: ("IG-88", "ig88"),
    31: ("Jawa", "human_small"),
    32: ("K-3PO", "c3po"),
    50: ("MSE-6", "mse6"),
    52: ("Obi-Wan Kenobi (Force Ghost)", "ghost_human"),
    53: ("R2-D2", "r2d2_r5d4"),
    54: ("R5-D4", "r2d2_r5d4"),
    55: ("Gonk Droid", "gonk"),
    59: ("Salacious Crumb", "salacious_crumb"),
    67: ("Wicket", "wicket"),
    69: ("Yoda", "human_small"),
    70: ("Yoda (Force Ghost)", "ghost_yoda"),
}

CAPE_ANIMATIONS = {
    31: (1165, 1147),
    69: (1165, 1194),
    70: (2463, 1194),
}

CAMERA_DIRECTIONS = {
    17: "0.35,0.75,1.55",
    31: "0.35,0.75,1.55",
}


def one_entry(asset_id: int) -> Path:
    matches = list((ROOT / "out" / "raw" / "archive" / "entries").glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise ValueError(f"asset {asset_id}: expected one archive entry, found {len(matches)}")
    return matches[0]


def glb_json(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    length, kind = struct.unpack_from("<I4s", data, 12)
    if kind != b"JSON":
        raise ValueError(f"{path}: first GLB chunk is not JSON")
    return json.loads(data[20 : 20 + length])


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


def convert_animated_model(target: Path, model_asset: int, animation_asset: int) -> Path:
    target.mkdir(parents=True, exist_ok=True)
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


def main() -> int:
    manifest = json.loads((COMPONENTS / "manifest.json").read_text(encoding="utf-8"))
    components = {row["runtime_id"]: row for row in manifest["runtime_appearances"]}
    OUTPUT.mkdir(parents=True, exist_ok=True)
    godot = ROOT / "tools" / "vendor" / "godot" / "Godot_v4.7.1-stable_win64.exe"
    project = ROOT / "tools" / "godot_runtime_composite_preview"
    appdata = ROOT / "out" / "work" / "godot_appdata"
    appdata.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["APPDATA"] = str(appdata)
    environment["LOCALAPPDATA"] = str(appdata)
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    rendered: list[tuple[int, str, Path]] = []
    audit_rows = []

    for runtime_id, (label, body_name) in ROWS.items():
        row = components[runtime_id]
        candidate_dir = OUTPUT / f"runtime_{runtime_id:03d}"
        if candidate_dir.exists():
            shutil.rmtree(candidate_dir)
        if runtime_id == 31:
            body = convert_animated_model(candidate_dir, 2835, 491)
        else:
            shutil.copytree(BODIES / body_name, candidate_dir)
            body = one_glb(candidate_dir)
        output = OUTPUT / "renders" / f"runtime_{runtime_id:03d}.png"
        command = [
            str(godot), "--display-driver", "windows", "--rendering-driver", "opengl3",
            "--audio-driver", "Dummy", "--path", str(project),
            "--log-file", str(ROOT / "out" / "work" / "godot_runtime_special.log"),
            "body_preview.tscn", "--", f"--body={body}", f"--output={output}",
        ]
        animations = glb_json(body).get("animations", [])
        if animations:
            command.append(f"--animation={animations[0].get('name', '')}")
        if runtime_id in CAMERA_DIRECTIONS:
            command.append(f"--camera-direction={CAMERA_DIRECTIONS[runtime_id]}")
        for part in row["parts"]:
            mode = part["mode"]
            texture = component_texture(runtime_id, part)
            model = part.get("model")
            if texture is not None and mode in (0, 1, 2):
                if body_name == "r2d2_r5d4":
                    shutil.copyfile(texture, candidate_dir / f"r2tex{mode + 1}.png")
                else:
                    command.append(f"--body-{part['slot']}-texture={texture}")
            if model is not None and mode in (0, 3, 4):
                if mode == 4 and runtime_id in CAPE_ANIMATIONS:
                    cape_model, cape_animation = CAPE_ANIMATIONS[runtime_id]
                    model_glb = convert_animated_model(
                        candidate_dir / "animated_back_gear", cape_model, cape_animation
                    )
                else:
                    model_glb = one_glb(COMPONENTS / "models" / f"asset_{model['asset']:04d}")
                slot = {0: "head", 3: "gear", 4: "back-gear"}[mode]
                command.append(f"--{slot}={model_glb}")
                if texture is not None:
                    command.append(f"--{slot}-texture={texture}")
        completed = subprocess.run(
            command, cwd=ROOT, env=environment, text=True, capture_output=True,
            creationflags=creationflags,
        )
        if completed.returncode != 0:
            print(completed.stdout)
            print(completed.stderr)
            raise RuntimeError(f"Godot failed for runtime {runtime_id} ({completed.returncode})")
        rendered.append((runtime_id, label, output))
        audit_rows.append(
            {
                "runtime_id": runtime_id,
                "label": label,
                "body": body_name,
                "body_animation_override": 491 if runtime_id == 31 else None,
                "cape_animation": CAPE_ANIMATIONS.get(runtime_id),
                "camera_direction": CAMERA_DIRECTIONS.get(runtime_id),
            }
        )
        print(f"[{len(rendered):02d}/{len(ROWS):02d}] runtime {runtime_id:03d} {label}", flush=True)

    columns = 4
    tile = (300, 250)
    label_height = 34
    sheet = Image.new("RGB", (columns * tile[0], ((len(rendered) + 3) // 4) * tile[1]), (12, 16, 25))
    draw = ImageDraw.Draw(sheet)
    for index, (runtime_id, label, path) in enumerate(rendered):
        x = index % columns * tile[0]
        y = index // columns * tile[1]
        image = Image.open(path).convert("RGB")
        image.thumbnail((tile[0], tile[1] - label_height), Image.Resampling.LANCZOS)
        sheet.paste(image, (x + (tile[0] - image.width) // 2, y + label_height + (tile[1] - label_height - image.height) // 2))
        draw.rectangle((x, y, x + tile[0] - 1, y + label_height - 1), fill=(28, 36, 54))
        draw.text((x + 7, y + 7), f"{runtime_id:02d}  {label}", fill=(235, 240, 250))
        draw.rectangle((x, y, x + tile[0] - 1, y + tile[1] - 1), outline=(63, 75, 99))
    sheet.save(OUTPUT / "contact_sheet.png")
    (OUTPUT / "audit_manifest.json").write_text(json.dumps({"rows": audit_rows}, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(rendered)} special-body candidates -> {OUTPUT / 'contact_sheet.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
