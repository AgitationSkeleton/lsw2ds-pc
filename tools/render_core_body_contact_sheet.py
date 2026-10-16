#!/usr/bin/env python3
"""Render every verified reusable/dedicated playable body into an audit sheet."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "out" / "exported" / "playable_core_bodies"
OUTPUT = ROOT / "out" / "work" / "core_body_candidates"


def main() -> int:
    config = json.loads((ROOT / "config" / "playable_core_bodies.json").read_text(encoding="utf-8"))
    godot = ROOT / "tools" / "vendor" / "godot" / "Godot_v4.7.1-stable_win64.exe"
    project = ROOT / "tools" / "godot_preview_v2"
    appdata = ROOT / "out" / "work" / "godot_appdata"
    OUTPUT.mkdir(parents=True, exist_ok=True)
    appdata.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["APPDATA"] = str(appdata)
    environment["LOCALAPPDATA"] = str(appdata)
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    rendered: list[tuple[str, Path]] = []
    for name in config["bodies"]:
        glbs = list((SOURCE / name).glob("*.glb"))
        if len(glbs) != 1:
            raise ValueError(f"{name}: expected one GLB, found {len(glbs)}")
        output = OUTPUT / f"{name}.png"
        command = [
            str(godot), "--display-driver", "windows", "--rendering-driver", "opengl3",
            "--audio-driver", "Dummy", "--path", str(project),
            "--log-file", str(ROOT / "out" / "work" / "godot_core_body.log"), "--",
            f"--input={glbs[0]}", f"--output={output}",
        ]
        completed = subprocess.run(
            command, cwd=ROOT, env=environment, text=True, capture_output=True,
            creationflags=creationflags,
        )
        if completed.returncode != 0:
            print(completed.stdout)
            print(completed.stderr)
            raise RuntimeError(f"Godot failed for {name} ({completed.returncode})")
        rendered.append((name, output))
        print(f"[{len(rendered):02d}/{len(config['bodies']):02d}] {name}", flush=True)

    columns = 4
    tile = (300, 240)
    label_height = 30
    row_count = (len(rendered) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * tile[0], row_count * tile[1]), (12, 16, 25))
    draw = ImageDraw.Draw(sheet)
    for index, (name, path) in enumerate(rendered):
        x = index % columns * tile[0]
        y = index // columns * tile[1]
        image = Image.open(path).convert("RGB")
        image.thumbnail((tile[0], tile[1] - label_height), Image.Resampling.LANCZOS)
        sheet.paste(
            image,
            (x + (tile[0] - image.width) // 2, y + label_height + (tile[1] - label_height - image.height) // 2),
        )
        draw.rectangle((x, y, x + tile[0] - 1, y + label_height - 1), fill=(28, 36, 54))
        draw.text((x + 8, y + 8), name, fill=(235, 240, 250))
        draw.rectangle((x, y, x + tile[0] - 1, y + tile[1] - 1), outline=(63, 75, 99))
    contact_sheet = OUTPUT / "contact_sheet.png"
    sheet.save(contact_sheet)
    print(f"Wrote {len(rendered)}-body audit sheet -> {contact_sheet}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
