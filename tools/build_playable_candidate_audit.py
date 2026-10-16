#!/usr/bin/env python3
"""Reproduce the ignored 67-target playable-character render audit."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def run(*arguments: str) -> None:
    subprocess.run([sys.executable, *arguments], cwd=ROOT, check=True)


def main() -> int:
    config = json.loads(
        (ROOT / "config" / "runtime_playable_candidate_rows.json").read_text(encoding="utf-8")
    )
    run(
        "tools/export_runtime_character_components.py",
        "out/raw/arm9_section_0_02000000.bin",
        "out/raw/archive/catalog.json",
    )
    run(
        "tools/build_runtime_humanoid_candidates.py",
        "--rows", *(str(row) for row in config["humanoid_rows"]),
    )
    run("tools/sanitize_runtime_candidate_manifest.py")
    run("tools/render_runtime_humanoid_candidates.py")
    run("tools/render_runtime_special_candidates.py")
    print(
        f"Built {len(config['humanoid_rows']) + len(config['special_body_rows'])} "
        "playable-target candidates in ignored out/work directories"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
