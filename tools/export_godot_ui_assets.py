#!/usr/bin/env python3
"""Export the ROM-derived UI/menu assets used by the Godot prototype."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
DEFAULT_OUTPUT = ROOT / "out" / "exported" / "godot_ui"
DEFAULT_APICULA = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"


def entry(entries: Path, asset_id: int) -> Path:
    matches = list(entries.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise RuntimeError(f"asset {asset_id}: expected one archive entry, found {matches}")
    return matches[0]


def convert(apicula: Path, entries: Path, output: Path, name: str, ids: list[int]) -> dict:
    target = output / name
    target.mkdir(parents=True, exist_ok=True)
    command = [
        str(apicula), "convert", *(str(entry(entries, asset_id)) for asset_id in ids),
        "-o", str(target), "-f", "glb", "--more-textures", "--overwrite",
        "--all-animations"
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    return {
        "name": name,
        "asset_ids": ids,
        "status": "exported" if completed.returncode == 0 else "failed",
        "files": sorted(path.name for path in target.iterdir() if path.is_file()),
        "diagnostic": (completed.stdout + completed.stderr).strip()
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", type=Path, default=DEFAULT_ENTRIES)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--apicula", type=Path, default=DEFAULT_APICULA)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    groups = [
        ("legal_stars", [3426, 3427, 3429]),
        ("legal_1", [3416, 3415]),
        ("legal_2", [3419, 3418]),
        ("legal_3", [3422, 3421, 3423]),
        ("id_bounty", [2861, 2855, 2856, 2857, 2858, 2859, 2860]),
        ("id_c3po", [2862, 2855, 2856, 2857, 2858, 2859, 2860]),
        ("id_cloud_city", [2863, 2855, 2856, 2857, 2858, 2859, 2860]),
        ("id_jabba", [2864, 2855, 2856, 2857, 2858, 2859, 2860]),
        ("id_r2d2", [2865, 2855, 2856, 2857, 2858, 2859, 2860]),
        ("id_stormtrooper", [2866, 2855, 2856, 2857, 2858, 2859, 2860]),
        ("bounty_detector", [684, 685, 686]),
        ("heart_candidate", [2634, 2635]),
        ("multiplayer_arrow", [3793]),
        ("jabbas_tower", [322, 324]),
        ("mos_eisley_streets", [327, 329]),
        # 954 IS THE HOLOGRAM SPIN and was missing. The Cantina Arena Lobby
        # family in the archive is:
        #     954  BCA0  CantinaArenaLobbJ   the SKELETAL animation
        #     955  BMD0  CantinaArenaLobb    the model
        #     956  BTA0  CantinaArenaLobbM   the texture (SRT0) animation
        #     957  BTX0  the textures
        # Exporting 955/956/957 gave a GLB whose only AnimationPlayer clip was
        # 0.001 s long with ZERO tracks, so the hologram - a bone in the room's
        # own 18-bone skeleton, "Hologram" at index 1 - never moved.
        ("bounty_hunter_lobby", [955, 954, 956, 957]),
        ("bounty_arena_turret", [332, 333, 334, 336, 339, 340, 341, 342, 343]),
        ("bounty_bridge", [747, 748, 750, 753, 755]),
        ("bounty_buildable_grapple", [818, 819, 820, 823]),
        ("bounty_grapple_point", [2522, 2523, 2524, 2525]),
        ("bounty_carbonite_objective", [2551, 2552, 2553, 2559]),
        ("bounty_lever_candidate", [1312, 1313, 1314, 1315, 1316, 1317, 1318, 1319, 1320]),
        ("extra_old_west_hat", [1555]),
        ("extra_old_west_weapon", [1483]),
        ("extra_medieval_weapon", [1494]),
        ("extra_medieval_crown", [1572]),
        ("extra_medieval_helmet", [3361]),
        # Old Timey. Decoded in doc 31: Extras flag 7 assigns gear part 228,
        # which is TopHat 5020. The Moustache (3781) is exported alongside it
        # because it sits in the same authored headwear block, but nothing in
        # the ROM applies it - see customization.gd.
        ("extra_old_timey_hat", [5020]),
        ("extra_old_timey_moustache_unused", [3781]),
        # Impact and explosion effects. The spark set carries one colour per
        # arena player slot, which is why they are exported together.
        ("fx_sparks", [548, 2539, 4164, 3980, 5211, 5414, 3814, 3847, 400]),
        ("fx_explosion_orange", [2214, 2216, 2217]),
        ("fx_explosion_purple", [2220, 2222, 2224]),
        ("fx_explosion_blue", [1895, 1898]),
        ("fx_smoke", [2227, 4447]),
        ("fx_blast_ring", [2276])
    ]
    results = [convert(args.apicula, args.entries, args.output, name, ids) for name, ids in groups]
    uv_results = []
    for name, asset_id, folder in [
        ("legal_stars_intro", 3427, "legal_stars"),
        ("legal_stars_loop", 3429, "legal_stars"),
        ("heart_uv", 2635, "heart_candidate")
    ]:
        destination = args.output / folder / f"{name}.json"
        command = [
            sys.executable, str(ROOT / "tools" / "extract_nsbta_uv.py"),
            str(entry(args.entries, asset_id)), "--output", str(destination)
        ]
        completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        uv_results.append({
            "name": name,
            "asset_id": asset_id,
            "status": "exported" if completed.returncode == 0 else "failed",
            "file": destination.name,
            "diagnostic": (completed.stdout + completed.stderr).strip()
        })
    font_completed = subprocess.run(
        [
            sys.executable, str(ROOT / "tools" / "export_custom_fonts.py"),
            "--entries", str(args.entries), "--output", str(args.output / "fonts")
        ],
        cwd=ROOT, text=True, capture_output=True
    )
    shutil.copyfile(ROOT / "config" / "ui_assets.json", args.output / "source_manifest.json")
    manifest = {
        "schema_version": 1,
        "rom_required": True,
        "groups": results,
        "material_animations": uv_results,
        "fonts": {
            "status": "exported" if font_completed.returncode == 0 else "failed",
            "diagnostic": (font_completed.stdout + font_completed.stderr).strip()
        }
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    failures = [result for result in results + uv_results if result["status"] != "exported"]
    if font_completed.returncode != 0:
        failures.append(manifest["fonts"])
    group_failures = sum(result["status"] != "exported" for result in results)
    print(
        f"Exported {len(results) - group_failures}/{len(results)} UI/model groups, "
        f"{len(uv_results)} material animations, and custom fonts -> {args.output}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
