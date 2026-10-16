"""Find assets by name, format or id - so the override pipeline is usable.

WHY. `tools/build_asset_rom.py` keys overrides on numeric `asset_id`, and nobody
knows that 4-LOM's head is asset 0. This turns the catalog into something you can
search, which is the difference between an asset pipeline that exists and one
anybody will actually use.

WHAT THE CATALOG KNOWS. 3,582 of the 5,548 entries carry names lifted out of the
Nitro containers themselves - model names, bone names, texture names - so the
game's own vocabulary is searchable:

    BMD0/MDL0  nsbmd   models        LOMHead, LOMMiniKit, BobaRifle
    BCA0/JNT0  nsbca   animations    LOMMiniKit_CompJ
    BTA0       nsbta   texture anim
    BVA0       nsbva   visibility anim
    BTX0       nsbtx   texture sets

Usage:
    python tools/asset_find.py bossk            # substring, case-insensitive
    python tools/asset_find.py --format nsbmd   # everything of one format
    python tools/asset_find.py --id 0           # one entry, in full
    python tools/asset_find.py --extract 0 out.bin
                                                # packed bytes, ready to edit
                                                # and drop into the override dir

A NOTE ON WHAT THE NAMES ARE. They are strings *inside* the asset, not filenames
the game looks up. They identify content reliably and they are not evidence of
how the game addresses it - two different questions, and only the first is
answered here.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CATALOG = ROOT / "out/raw/archive/catalog.json"


def load():
    if not CATALOG.exists():
        print("no catalog at %s" % CATALOG)
        sys.exit(1)
    return json.loads(CATALOG.read_text(encoding="utf-8"))


def archive_bytes():
    manifest = json.loads((ROOT / "out/raw/manifest.json").read_text(encoding="utf-8"))
    a = next(f for f in manifest["files"] if f["path"] == "rom.bin")
    rom = sorted(ROOT.glob("rom/*.nds"))[0].read_bytes()
    return rom[a["offset"]:a["end"]]


def show(e, verbose=False):
    names = e.get("names") or []
    head = ", ".join(str(n) for n in names[:5])
    if len(names) > 5:
        head += ", +%d more" % (len(names) - 5)
    print("id=%-5d %-6s %-6s %8d bytes  %s"
          % (e["asset_id"], e.get("extension") or "?", e.get("magic") or "?",
             e["packed_size"], head))
    if verbose:
        for k in ("archive_offset", "packed_size", "expanded_size", "wrapper",
                  "packed_sha256", "expanded_sha256", "output"):
            if k in e:
                print("    %-16s %s" % (k, e[k]))
        if names:
            print("    names            %s" % ", ".join(str(n) for n in names))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("query", nargs="?", help="substring to match against names")
    ap.add_argument("--format", help="filter by extension, e.g. nsbmd")
    ap.add_argument("--id", type=int, help="show one entry in full")
    ap.add_argument("--extract", nargs=2, metavar=("ID", "OUT"),
                    help="write an asset's packed bytes to a file")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    catalog = load()
    entries = catalog["entries"]

    if args.extract:
        aid = int(args.extract[0])
        e = entries[aid]
        data = archive_bytes()[e["archive_offset"]:
                               e["archive_offset"] + e["packed_size"]]
        out = pathlib.Path(args.extract[1])
        out.write_bytes(data)
        print("asset %d -> %s (%d bytes, %s)"
              % (aid, out, len(data), e.get("extension")))
        print("edit it and drop it in port/assets/override/%d.bin" % aid)
        return 0

    if args.id is not None:
        show(entries[args.id], verbose=True)
        return 0

    hits = entries
    if args.format:
        want = args.format.lower().lstrip(".")
        hits = [e for e in hits if (e.get("extension") or "").lower() == want]
    if args.query:
        q = args.query.lower()
        hits = [e for e in hits
                if any(q in str(n).lower() for n in (e.get("names") or []))]

    if not hits:
        print("nothing matched")
        return 1
    for e in hits[:args.limit]:
        show(e)
    if len(hits) > args.limit:
        print("... %d more (use --limit)" % (len(hits) - args.limit))
    print("\n%d match(es)" % len(hits))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
