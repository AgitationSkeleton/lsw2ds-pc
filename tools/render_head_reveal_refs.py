#!/usr/bin/env python3
"""Game-accurate reference renders of Boba Fett and Jango Fett, normal and with a
hat applied so their head is revealed.

WHY THIS EXISTS. The fork's head fix re-attaches Boba's/Jango's head when a
non-covering hat replaces their stock helmet, but that state is almost
impossible to see in the running port: the arena lobby's follow-camera only
shows the character's back, and the hat blob hides the head from that angle. So
this renders the SAME composite the game uses - the extracted body + head + gear
(helmet or hat) + jetpack - from a front camera, giving a ground-truth picture
of what "head revealed under a hat" should look like.

HOW. It reuses the Godot composite pipeline (tools/godot_runtime_composite_preview,
bounty_preview.gd) and the exact per-character parts the runtime manifest records
(head texture 3633, legs/torso textures, the BobaFettHelmet gear asset 555, the
BobaPack jetpack). "normal" attaches the helmet gear over the head; each hat
variant swaps that gear for an extras hat GLB (CowboyHat / TopHat / KnightHelmet)
so the head textured with the face shows beneath it - which is precisely what the
in-game head-fix produces.

Output: out/work/head_reveal_refs/<key>_<variant>.png plus a contact sheet.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out" / "work" / "head_reveal_refs"
COMPONENTS = ROOT / "out" / "exported" / "runtime_character_components"
WEAPONS = ROOT / "out" / "exported" / "bounty_hunter_weapons"
MODEL_ROOT = COMPONENTS / "models"
GODOT = ROOT / "tools" / "vendor" / "godot" / "Godot_v4.7.1-stable_win64.exe"
PROJECT = ROOT / "tools" / "godot_runtime_composite_preview"

# Reuse the audit module's body-prep and helpers rather than copy them.
_spec = importlib.util.spec_from_file_location(
    "bh_audit", ROOT / "tools" / "render_bounty_hunter_weapon_audit.py"
)
audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(audit)

# The hats a host can put on a hunter (the extras' own headgear, exported from
# the ROM). One is enough to reveal the head; render a couple for choice.
HATS = {
    "cowboyhat": (ROOT / "out/exported/godot_ui/extra_old_west_hat/CowboyHat.glb",
                  ROOT / "out/exported/godot_ui/extra_old_west_hat/Brown.png"),
    "tophat": (ROOT / "out/exported/godot_ui/extra_old_timey_hat/TopHat.glb",
               ROOT / "out/exported/godot_ui/extra_old_timey_hat/Black.png"),
}

CHARACTERS = ["boba_fett", "jango_fett"]


def build(config, human_rows, component_rows, key, variant, body):
    """Return (output_path, godot argv) for one character/variant.

    `body` is the prepared human body GLB (built once per character)."""
    char = config[key]
    rid = char["runtime_id"]
    source = component_rows[rid]
    output = OUT / f"{key}_{variant}.png"

    cmd = [
        str(GODOT), "--display-driver", "windows", "--rendering-driver", "opengl3",
        "--audio-driver", "Dummy", "--path", str(PROJECT),
        "--log-file", str(OUT / "godot.log"), "bounty_preview.tscn", "--",
        f"--body={body}", f"--output={output}", f"--animation={char['idle_name']}",
    ]

    for part in source["parts"]:
        mode = part["mode"]
        texture = audit.component_texture(rid, part)
        model = part.get("model")
        # Body-slot textures (head face, legs, torso) always applied.
        if texture is not None and mode in (0, 1, 2):
            cmd.append(f"--body-{part['slot']}-texture={texture}")
        # Jetpack (back-gear) always attached.
        if model is not None and mode == 4:
            mp = audit.one_glb(MODEL_ROOT / f"asset_{model['asset']:04d}")
            cmd.append(f"--back-gear={mp}")
            if texture is not None:
                cmd.append(f"--back-gear-texture={texture}")
        # Gear (mode 3) is the stock HELMET. Keep it for "normal"; the hat
        # variants replace it below so the head shows.
        if model is not None and mode == 3 and variant == "normal":
            mp = audit.one_glb(MODEL_ROOT / f"asset_{model['asset']:04d}")
            cmd.append(f"--gear={mp}")
            if texture is not None:
                cmd.append(f"--gear-texture={texture}")

    if variant != "normal":
        hat_glb, hat_tex = HATS[variant]
        cmd.append(f"--gear={hat_glb}")
        cmd.append(f"--gear-texture={hat_tex}")

    weapon = audit.one_glb(WEAPONS / f"asset_{char['weapon_asset']:04d}")
    for hand in char["hands"]:
        cmd.append(f"--weapon-{hand}={weapon}")
        if char.get("weapon_roll_degrees"):
            cmd.append(f"--weapon-roll-{hand}={char['weapon_roll_degrees']}")
    return output, cmd


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / "config" / "bounty_hunter_weapons.json").read_text("utf-8"))["characters"]
    human_manifest = json.loads(
        (ROOT / "out/work/runtime_character_candidates/render_manifest.json").read_text("utf-8")
    )
    component_manifest = json.loads((COMPONENTS / "manifest.json").read_text("utf-8"))
    human_rows = {r["runtime_id"]: r for r in human_manifest["rows"]}
    component_rows = {r["runtime_id"]: r for r in component_manifest["runtime_appearances"]}

    appdata = ROOT / "out" / "work" / "godot_appdata"
    appdata.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["APPDATA"] = str(appdata)
    env["LOCALAPPDATA"] = str(appdata)
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    variants = ["normal"] + list(HATS.keys())
    tiles = []
    for key in CHARACTERS:
        body = audit.prepare_human_body(key, human_rows[config[key]["runtime_id"]],
                                        config[key]["idle_animation"])
        for variant in variants:
            output, cmd = build(config, human_rows, component_rows, key, variant, body)
            done = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                                  capture_output=True, creationflags=creationflags)
            if done.returncode != 0:
                print(done.stdout)
                print(done.stderr)
                raise RuntimeError(f"Godot failed for {key}/{variant} ({done.returncode})")
            print(f"rendered {key} / {variant} -> {output.relative_to(ROOT)}", flush=True)
            tiles.append((f"{config[key]['label']}  |  {variant}", output))

    # Contact sheet: one row per character, columns = variants.
    cols = len(variants)
    rows = len(CHARACTERS)
    tw, th, lh = 360, 380, 34
    sheet = Image.new("RGB", (cols * tw, rows * th), (12, 16, 25))
    d = ImageDraw.Draw(sheet)
    for i, (label, path) in enumerate(tiles):
        x = (i % cols) * tw
        y = (i // cols) * th
        im = Image.open(path).convert("RGB")
        im.thumbnail((tw, th - lh), Image.Resampling.LANCZOS)
        sheet.paste(im, (x + (tw - im.width) // 2, y + lh + (th - lh - im.height) // 2))
        d.rectangle((x, y, x + tw - 1, y + lh - 1), fill=(28, 36, 54))
        d.text((x + 8, y + 10), label, fill=(235, 240, 250))
        d.rectangle((x, y, x + tw - 1, y + th - 1), outline=(63, 75, 99))
    sheet.save(OUT / "contact_sheet.png")
    print(f"contact sheet -> {(OUT / 'contact_sheet.png').relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
