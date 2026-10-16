#!/usr/bin/env python3
"""Publish every generated playable-character render into renders/.

The approval gate still applies: approved renders keep their canonical place in
``renders/characters`` and are never overwritten here. Everything else is copied
into ``renders/characters/candidates`` so the whole roster is reviewable in one
place without silently promoting it.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
HUMANOID = ROOT / "out" / "work" / "runtime_character_candidates"
SPECIAL = ROOT / "out" / "work" / "runtime_special_candidates"
RENDERS = ROOT / "renders" / "characters"
CANDIDATES = RENDERS / "candidates"


def slug(label: str) -> str:
    value = re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_")
    return value or "unnamed"


def approved_slugs() -> set[str]:
    config = json.loads(
        (ROOT / "config" / "render_approval.json").read_text(encoding="utf-8")
    )
    return set(config.get("approved_character_renders", {}))


def collect(manifest: Path, render_dir: Path) -> list[dict]:
    if not manifest.is_file():
        return []
    rows = json.loads(manifest.read_text(encoding="utf-8")).get("rows", [])
    collected = []
    for row in rows:
        runtime_id = int(row["runtime_id"])
        source = render_dir / f"runtime_{runtime_id:03d}.png"
        if not source.is_file():
            continue
        collected.append(
            {
                "runtime_id": runtime_id,
                "label": row.get("label", f"runtime_{runtime_id:03d}"),
                "source": source,
            }
        )
    return collected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--clean",
        action="store_true",
        help="remove previously published candidates before copying",
    )
    args = parser.parse_args()

    approved = approved_slugs()
    if args.clean and CANDIDATES.is_dir():
        shutil.rmtree(CANDIDATES)
    CANDIDATES.mkdir(parents=True, exist_ok=True)

    rows = collect(HUMANOID / "render_manifest.json", HUMANOID / "renders")
    rows += collect(SPECIAL / "audit_manifest.json", SPECIAL / "renders")
    rows.sort(key=lambda row: row["runtime_id"])

    published: list[dict] = []
    skipped: list[dict] = []
    for row in rows:
        name = slug(row["label"])
        if name in approved:
            # The canonical approved render stays authoritative.
            skipped.append({"runtime_id": row["runtime_id"], "label": row["label"]})
            continue
        target = CANDIDATES / f"{name}.png"
        shutil.copyfile(row["source"], target)
        published.append(
            {
                "runtime_id": row["runtime_id"],
                "label": row["label"],
                "path": target.relative_to(ROOT).as_posix(),
            }
        )

    manifest = {
        "schema_version": 1,
        "status": "candidate renders awaiting visual approval",
        "approved_untouched": sorted(approved),
        "published_count": len(published),
        "published": published,
        "skipped_because_approved": skipped,
    }
    (RENDERS / "candidates_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"Published {len(published)} candidate renders -> {CANDIDATES} "
        f"({len(skipped)} left to the approved directory)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
