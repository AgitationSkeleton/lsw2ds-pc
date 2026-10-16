#!/usr/bin/env python3
"""Export every verified reusable playable body rig to local GLB/PNG data."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = ROOT / "config" / "playable_core_bodies.json"
DEFAULT_ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
DEFAULT_OUTPUT = ROOT / "out" / "exported" / "playable_core_bodies"
DEFAULT_APICULA = ROOT / "tools" / "vendor" / "apicula" / "apicula.exe"


def entry(entries: Path, asset_id: int) -> Path:
    matches = list(entries.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise RuntimeError(f"asset {asset_id}: expected one extracted file, found {matches}")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--entries", type=Path, default=DEFAULT_ENTRIES)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--apicula", type=Path, default=DEFAULT_APICULA)
    args = parser.parse_args()

    config = json.loads(args.config.read_text(encoding="utf-8"))
    results: list[dict] = []
    for name, body in config["bodies"].items():
        inputs = [entry(args.entries, body["model"])]
        if "texture_bank" in body:
            inputs.append(entry(args.entries, body["texture_bank"]))
        if "idle_animation" in body:
            inputs.append(entry(args.entries, body["idle_animation"]))
        target = args.output / name
        target.mkdir(parents=True, exist_ok=True)
        command = [
            str(args.apicula), "convert", *(str(path) for path in inputs),
            "-o", str(target), "-f", "glb", "--more-textures", "--overwrite",
        ]
        if "idle_animation" in body:
            command.append("--all-animations")
        completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        result = {
            "name": name,
            "model": body["model"],
            "idle_animation": body.get("idle_animation"),
            "status": "exported" if completed.returncode == 0 else "failed",
            "diagnostic": (completed.stdout + completed.stderr).strip(),
        }
        results.append(result)
        print(f"{name}: {result['status']}")

    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "manifest.json").write_text(
        json.dumps({"schema_version": 1, "bodies": results}, indent=2) + "\n",
        encoding="utf-8",
    )
    failures = [result for result in results if result["status"] != "exported"]
    print(f"Exported {len(results) - len(failures)}/{len(results)} verified body rigs")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
