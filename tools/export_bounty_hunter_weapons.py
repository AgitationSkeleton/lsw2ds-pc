#!/usr/bin/env python3
"""Export the corroborated Bounty Hunter primary weapon models to GLB."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def one_entry(entries: Path, asset_id: int) -> Path:
    matches = list(entries.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise ValueError(f"asset {asset_id}: expected one archive entry, found {len(matches)}")
    return matches[0]


def main() -> int:
    config = json.loads((ROOT / "config" / "bounty_hunter_weapons.json").read_text(encoding="utf-8"))
    entries = ROOT / "out" / "raw" / "archive" / "entries"
    output = ROOT / "out" / "exported" / "bounty_hunter_weapons"
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"
    assets = sorted(
        {character["weapon_asset"] for character in config["characters"].values()}
        | {character["secondary_asset"] for character in config["characters"].values() if "secondary_asset" in character}
    )
    results = []
    for asset_id in assets:
        target = output / f"asset_{asset_id:04d}"
        target.mkdir(parents=True, exist_ok=True)
        completed = subprocess.run(
            [
                str(apicula), "convert", str(one_entry(entries, asset_id)),
                "-o", str(target), "-f", "glb", "--more-textures", "--overwrite",
            ],
            cwd=ROOT, text=True, capture_output=True,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
        glbs = list(target.glob("*.glb"))
        if len(glbs) != 1:
            raise ValueError(f"asset {asset_id}: expected one exported GLB, found {len(glbs)}")
        results.append({"asset": asset_id, "glb": glbs[0].name})
        print(f"weapon asset {asset_id:04d} -> {glbs[0]}")
    output.mkdir(parents=True, exist_ok=True)
    (output / "manifest.json").write_text(json.dumps({"weapons": results}, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {len(results)} distinct weapon models -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
