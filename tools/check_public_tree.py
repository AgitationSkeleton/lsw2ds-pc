"""Fail if the repository holds something it must never publish.

    python3 tools/check_public_tree.py

Run by CI on every push and by the release workflow. It checks the files git
tracks (not your local rom/, out/ or build/, which .gitignore keeps out):

  * no game, BIOS, firmware or save images, executables, logs or dumps - the
    one save allowed is port/saves/unlocked-template.sav, which seeds the
    unlocks and holds no game data;
  * nothing shaped like a credential: Discord webhooks, GitHub tokens, private
    keys, Slack and AWS keys. The master server's webhooks are Cloudflare
    secrets (services/master-server/README.md), never files;
  * no file over 5 MB: game data is the likeliest thing to be that large.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FORBIDDEN_SUFFIXES = {".nds", ".srl", ".dsi", ".rom", ".sav", ".log", ".exe",
                      ".dll", ".sqlite3", ".dmp", ".ml1", ".ml2", ".ml3", ".ml4",
                      ".ml5", ".ppm", ".lsw2font"}
ALLOWED = {"port/saves/unlocked-template.sav"}
MAX_BYTES = 5_000_000
SECRETS = [
    re.compile(rb"discord(?:app)?\.com/api/webhooks/\d+/[A-Za-z0-9_-]{20,}"),
    re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{30,}"),
    re.compile(rb"github_pat_[A-Za-z0-9_]{30,}"),
    re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(rb"\bxox[abpr]-[A-Za-z0-9-]{10,}"),
    re.compile(rb"\bAKIA[0-9A-Z]{16}\b"),
]


def main() -> int:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True,
                         check=True).stdout.decode("utf-8")
    bad = []
    files = [f for f in out.split("\0") if f]
    for rel in files:
        path = ROOT / rel
        if not path.is_file():
            continue
        if rel not in ALLOWED and path.suffix.lower() in FORBIDDEN_SUFFIXES:
            bad.append(f"{rel}: this file type is never committed")
            continue
        size = path.stat().st_size
        if size > MAX_BYTES:
            bad.append(f"{rel}: {size / 1e6:.1f} MB - too large to be source")
            continue
        data = path.read_bytes()
        for pat in SECRETS:
            if pat.search(data):
                bad.append(f"{rel}: contains something shaped like a credential")
                break
    if bad:
        print("public-tree audit FAILED:")
        for line in bad:
            print("  " + line)
        return 1
    print(f"public-tree audit: {len(files)} tracked files, nothing that must not be published")
    return 0


if __name__ == "__main__":
    sys.exit(main())
