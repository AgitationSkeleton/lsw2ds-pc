#!/usr/bin/env python3
"""Build a Human_Model candidate with explicit per-material runtime textures."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent

MATERIAL_TEXTURES = {
    "head": "HeadGeneric.png",
    "legs": "LegsGeneric.png",
    "torso": "TorsoGeneric.png",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    for material in MATERIAL_TEXTURES:
        parser.add_argument(
            f"--{material}-texture",
            type=Path,
            help=f"decoded replacement for the {material} material",
        )
    parser.add_argument("--animation", type=int, default=491)
    parser.add_argument("--output-root", type=Path, default=Path("out/work/humanoid_candidates"))
    args = parser.parse_args()

    entries = ROOT / "out" / "raw" / "archive" / "entries"
    apicula = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"
    target = ROOT / args.output_root / args.name
    target.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            str(apicula), "convert",
            str(entries / "2836.nsbmd"), str(entries / "2837.nsbtx"),
            str(next(entries.glob(f"{args.animation:04d}.*"))),
            "-o", str(target), "-f", "glb", "--more-textures", "--overwrite",
            "--all-animations",
        ],
        cwd=ROOT,
        check=True,
    )
    copied = []
    for material, filename in MATERIAL_TEXTURES.items():
        replacement = getattr(args, f"{material}_texture")
        if replacement is None:
            continue
        source = (ROOT / replacement).resolve()
        shutil.copyfile(source, target / filename)
        copied.append(f"{material}={source.name}")
    print(f"Built candidate {args.name}: {target / 'Human_Model.glb'}")
    print("Runtime material replacements: " + (", ".join(copied) if copied else "none"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
