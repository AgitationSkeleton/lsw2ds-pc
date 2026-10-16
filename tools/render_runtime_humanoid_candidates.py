#!/usr/bin/env python3
"""Render prepared runtime humanoids and create a labeled audit sheet."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "manifest", type=Path, nargs="?",
        default=Path("out/work/runtime_character_candidates/render_manifest.json"),
    )
    parser.add_argument(
        "--contact-sheet", type=Path,
        default=Path("out/work/runtime_character_candidates/contact_sheet.png"),
    )
    args = parser.parse_args()

    manifest = json.loads((ROOT / args.manifest).read_text(encoding="utf-8"))
    godot = ROOT / "tools" / "vendor" / "godot" / "Godot_v4.7.1-stable_win64.exe"
    project = ROOT / "tools" / "godot_runtime_composite_preview"
    appdata = ROOT / "out" / "work" / "godot_appdata"
    appdata.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["APPDATA"] = str(appdata)
    environment["LOCALAPPDATA"] = str(appdata)
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    rendered: list[tuple[dict[str, object], Path]] = []
    for index, row in enumerate(manifest["rows"], 1):
        output = Path(row["output"])
        command = [
            str(godot), "--display-driver", "windows", "--rendering-driver", "opengl3",
            "--audio-driver", "Dummy", "--path", str(project),
            "--log-file", str(ROOT / "out" / "work" / "godot_runtime_composite.log"),
            "world_back_preview.tscn", "--", f"--body={row['body']}",
            f"--output={output}", f"--animation={row['animation']}",
        ]
        for key in ("head", "head_texture", "gear", "gear_texture", "back_gear", "back_gear_texture"):
            if key in row:
                command.append(f"--{key.replace('_', '-')}={row[key]}")
        if row.get("back_gear_space"):
            command.append(f"--back-gear-space={row['back_gear_space']}")
        completed = subprocess.run(
            command, cwd=ROOT, env=environment, text=True, capture_output=True,
            creationflags=creationflags,
        )
        if completed.returncode != 0:
            print(completed.stdout)
            print(completed.stderr)
            raise RuntimeError(f"Godot failed for runtime {row['runtime_id']} ({completed.returncode})")
        rendered.append((row, output))
        print(f"[{index:02d}/{len(manifest['rows']):02d}] runtime {row['runtime_id']:03d} {row['label']}", flush=True)

    columns = 5
    tile = (256, 216)
    label_height = 28
    rows_count = (len(rendered) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * tile[0], rows_count * tile[1]), (12, 16, 25))
    draw = ImageDraw.Draw(sheet)
    for index, (row, output) in enumerate(rendered):
        x = index % columns * tile[0]
        y = index // columns * tile[1]
        image = Image.open(output).convert("RGB")
        image.thumbnail((tile[0], tile[1] - label_height), Image.Resampling.LANCZOS)
        offset_x = x + (tile[0] - image.width) // 2
        offset_y = y + label_height + (tile[1] - label_height - image.height) // 2
        sheet.paste(image, (offset_x, offset_y))
        label = f"{row['runtime_id']:02d}  {row['label']}"
        draw.rectangle((x, y, x + tile[0] - 1, y + label_height - 1), fill=(28, 36, 54))
        draw.text((x + 7, y + 7), label, fill=(235, 240, 250))
        draw.rectangle((x, y, x + tile[0] - 1, y + tile[1] - 1), outline=(63, 75, 99))
    contact_sheet = (ROOT / args.contact_sheet).resolve()
    contact_sheet.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(contact_sheet)
    print(f"Wrote {len(rendered)}-character audit sheet -> {contact_sheet}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
