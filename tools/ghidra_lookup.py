"""Look something up in the pre-decompiled corpus - no Ghidra run required.

tools/ghidra_decompile_all.ps1 writes the whole ARM9 image to
out/ghidra/decomp/ once. This reads it, so a question that used to cost a
multi-minute headless invocation costs a file read.

    python tools/ghidra_lookup.py 0x0203764C          the function at an address
    python tools/ghidra_lookup.py 0x0203764C -c       ... with its C body
    python tools/ghidra_lookup.py --callers 0x02007268    who calls it
    python tools/ghidra_lookup.py --calls 0x0203764C      what it calls
    python tools/ghidra_lookup.py --grep "0x020F03AC"     every body mentioning it
    python tools/ghidra_lookup.py --refs 0x020EEA28       functions touching a datum
    python tools/ghidra_lookup.py --strings Carbonite     the string dump
    python tools/ghidra_lookup.py --stats                 what the corpus holds

AN ADDRESS INSIDE A FUNCTION RESOLVES TO THAT FUNCTION. A repeated mistake on
this project is treating a mid-body address as an entry point - a function's
entry is its push, which may be several instructions earlier - so a lookup that
does not land on an entry reports the containing function and says by how much
it missed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# BOTH corpora. A first pass covered only section 0 and left ITCM absent, even
# though real code lives there - the animation-event opcode dispatcher at
# 0x01FFAE50 and the step-up helper at 0x01FF9188 were both read out of it by
# hand. Every query searches both, so an ITCM address resolves like any other.
DECOMP = ROOT / "out" / "ghidra" / "decomp"
DECOMP_ITCM = ROOT / "out" / "ghidra" / "decomp_itcm"
CORPORA = [DECOMP, DECOMP_ITCM]


def load_index() -> list[dict]:
    rows = []
    for corpus in CORPORA:
        path = corpus / "index.jsonl"
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line:
                row = json.loads(line)
                # Remember which corpus a row came from so its body can be found.
                row["_corpus"] = str(corpus)
                rows.append(row)
    if not rows:
        raise SystemExit(
            "no corpus under %s\n"
            "run:  powershell -ExecutionPolicy Bypass -File "
            "tools/ghidra_decompile_all.ps1" % DECOMP.parent)
    return rows


def body_path(row: dict) -> Path:
    return Path(row.get("_corpus", str(DECOMP))) / row["file"]


def load_callers() -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for corpus in CORPORA:
        path = corpus / "callers.jsonl"
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line:
                row = json.loads(line)
                key = row["address"].lower()
                # A target can be called from both sections, so merge.
                merged = out.setdefault(key, [])
                for a in row.get("callers", []):
                    if a not in merged:
                        merged.append(a)
    return out


def norm(value: str) -> int:
    return int(value, 16) if value.lower().startswith("0x") else int(value, 16)


def containing(rows: list[dict], address: int) -> dict | None:
    best = None
    for row in rows:
        start = int(row["address"], 16)
        if start <= address < start + int(row.get("size", 0)):
            if best is None or start > int(best["address"], 16):
                best = row
    return best


def show(row: dict, address: int | None, body: bool) -> None:
    start = int(row["address"], 16)
    print("%s  %s  size=%d  params=%s"
          % (row["address"], row["name"], row.get("size", 0),
             row.get("params", "?")))
    if address is not None and address != start:
        print("  NOTE: 0x%08x is %d bytes INTO this function, not its entry."
              % (address, address - start))
        print("        A function's entry is its push, which is often earlier "
              "than an address quoted from a disassembly window.")
    calls = row.get("calls", [])
    if calls:
        print("  calls: %s" % " ".join(calls))
    if body:
        path = body_path(row)
        if path.exists():
            print()
            print(path.read_text(encoding="utf-8", errors="replace"))
        else:
            print("  (no body at %s)" % path)


def main() -> int:
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("address", nargs="?")
    p.add_argument("-c", "--body", action="store_true", help="print the C body")
    p.add_argument("--callers", help="who calls this address")
    p.add_argument("--calls", help="what this function calls")
    p.add_argument("--grep", help="regex over every decompiled body")
    p.add_argument("--refs", help="functions whose body mentions this datum")
    p.add_argument("--strings", help="substring search of the string dump")
    p.add_argument("--name", help="substring search of function names")
    p.add_argument("--stats", action="store_true")
    args = p.parse_args()

    if args.strings is not None:
        needle = args.strings.lower()
        hits = 0
        for corpus in CORPORA:
            path = corpus / "strings.txt"
            if not path.exists():
                continue
            for line in path.read_text(encoding="utf-8",
                                       errors="replace").splitlines():
                if needle in line.lower():
                    print(line)
                    hits += 1
        print("\n%d string(s)" % hits, file=sys.stderr)
        return 0

    rows = load_index()

    if args.stats:
        total_size = sum(int(r.get("size", 0)) for r in rows)
        named = sum(1 for r in rows if not r["name"].startswith("FUN_"))
        print("functions      %d" % len(rows))
        print("named          %d" % named)
        print("bytes covered  %d (0x%X)" % (total_size, total_size))
        if rows:
            lo = min(int(r["address"], 16) for r in rows)
            hi = max(int(r["address"], 16) + int(r.get("size", 0)) for r in rows)
            print("range          0x%08X .. 0x%08X" % (lo, hi))
        callers = load_callers()
        print("call targets   %d" % len(callers))
        total_strings = 0
        for corpus in CORPORA:
            sdump = corpus / "strings.txt"
            if sdump.exists():
                total_strings += len(sdump.read_text(
                    encoding="utf-8", errors="replace").splitlines())
        print("strings        %d" % total_strings)
        for corpus in CORPORA:
            n = sum(1 for r in rows if r.get("_corpus") == str(corpus))
            print("  %-14s %d functions" % (corpus.name, n))
        return 0

    if args.name:
        needle = args.name.lower()
        for row in rows:
            if needle in row["name"].lower():
                print("%s  %s  size=%d"
                      % (row["address"], row["name"], row.get("size", 0)))
        return 0

    if args.callers:
        target = "0x%08x" % norm(args.callers)
        callers = load_callers()
        who = callers.get(target, [])
        print("%s is called by %d site(s)" % (target, len(who)))
        by_addr = {r["address"].lower(): r for r in rows}
        for a in who:
            row = by_addr.get(a.lower())
            print("  %s  %s" % (a, row["name"] if row else "?"))
        return 0

    if args.calls:
        target = "0x%08x" % norm(args.calls)
        by_addr = {r["address"].lower(): r for r in rows}
        row = by_addr.get(target)
        if row is None:
            row = containing(rows, norm(args.calls))
        if row is None:
            raise SystemExit("no function at or containing %s" % target)
        print("%s %s calls:" % (row["address"], row["name"]))
        for a in row.get("calls", []):
            other = by_addr.get(a.lower())
            print("  %s  %s" % (a, other["name"] if other else "?"))
        return 0

    if args.grep or args.refs:
        pattern = args.grep if args.grep else re.escape(args.refs)
        rx = re.compile(pattern, re.IGNORECASE)
        hits = 0
        for row in rows:
            path = body_path(row)
            if not path.exists():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            matched = [l.strip() for l in text.splitlines() if rx.search(l)]
            # The header line carries the address and the call list, so it
            # matches almost everything - drop it unless it is the only hit.
            body_hits = [l for l in matched if not l.startswith("//")]
            if not body_hits:
                continue
            hits += 1
            print("%s  %s" % (row["address"], row["name"]))
            for line in body_hits[:6]:
                print("    %s" % line)
            if len(body_hits) > 6:
                print("    ... %d more" % (len(body_hits) - 6))
        print("\n%d function(s)" % hits, file=sys.stderr)
        return 0

    if not args.address:
        p.print_help()
        return 1

    address = norm(args.address)
    by_addr = {r["address"].lower(): r for r in rows}
    row = by_addr.get("0x%08x" % address)
    if row is not None:
        show(row, address, args.body)
        return 0
    row = containing(rows, address)
    if row is None:
        raise SystemExit("no function at or containing 0x%08x" % address)
    show(row, address, args.body)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
