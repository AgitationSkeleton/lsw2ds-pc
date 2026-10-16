#!/usr/bin/env python3
"""Remove state-default actor parts from a prepared candidate manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "manifest", type=Path, nargs="?",
        default=Path("out/work/runtime_character_candidates/render_manifest.json"),
    )
    parser.add_argument(
        "rules", type=Path, nargs="?",
        default=Path("config/runtime_attachment_state_defaults.json"),
    )
    args = parser.parse_args()
    manifest_path = (ROOT / args.manifest).resolve()
    rules = json.loads((ROOT / args.rules).read_text(encoding="utf-8"))["suppressed_default_parts"]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    suppressed = []
    for row in manifest["rows"]:
        for part in row["parts"]:
            rule = rules.get(str(part["actor_part"]))
            if rule is None:
                continue
            slot = rule["slot"]
            row.pop(slot, None)
            row.pop(f"{slot}_texture", None)
            row.setdefault("suppressed_default_parts", []).append(part["actor_part"])
            suppressed.append((row["runtime_id"], part["actor_part"], slot))
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for runtime_id, actor_part, slot in suppressed:
        print(f"runtime {runtime_id:03d}: suppressed default part {actor_part} ({slot})")
    print(f"Sanitized {manifest_path}: {len(suppressed)} state-default part(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
