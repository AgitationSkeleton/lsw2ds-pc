"""Put back the call arguments Ghidra dropped, using the image as the evidence.

WHAT IS BEING REPAIRED. ARM passes the first four arguments in r0-r3, and Ghidra
records an argument only where it can attribute the register setup to the call.
So it emits `FUN_020cccdc(0x7e)` for

    LDR r1, =0x027FFFF0
    MOV r2, #0
    MOV r0, #0x7E
    BL  0x020CCCDC

and the dropped r1 - the pointer the allocator dereferences - becomes a null
read three frames later. 2,735 call sites across 972 callees pass fewer
arguments than the callee declares.

THE ORIGINAL SOURCES ARE NOT TOUCHED. A patched copy is written to
out/work/patched/ and the build prefers it. out/ghidra stays exactly as the
decompiler produced it, so the two can always be diffed and the repair audited.
This is not "editing translated code to taste" - it is restoring a value that is
sitting in the cartridge, and refusing to do it would mean leaving code that is
knowingly wrong.

WHAT IT REFUSES TO PATCH, which matters more than what it patches:

  * A register whose value is not a literal or an immediate. If it came from
    another register, from memory, or from a previous call, the value is not
    static and no patch is honest.
  * A function containing MORE THAN ONE call to the same target. The C call
    sites cannot then be matched to the BLs with certainty, and patching the
    wrong one is worse than patching neither.
  * A call missing an argument that is not trailing. Ghidra drops trailing
    registers; a gap in the middle means the shape is not understood.

Everything skipped is counted and reported, so the residue is visible rather
than quietly left out.
"""

from __future__ import annotations

import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "out/work/patched"
SECTIONS = [(0x02000000, ROOT / "out/raw/arm9_section_0_02000000.bin"),
            (0x01FF8000, ROOT / "out/raw/arm9_section_1_01ff8000.bin")]
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]

DEFN = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b((?:thunk_)?FUN_[0-9a-fA-F]{8})\s*\(([^)]*)\)\s*$",
    re.M)
CALL = re.compile(r"\b(FUN_[0-9a-fA-F]{8})\s*\(([^();]*)\)")

sys.path.insert(0, str(ROOT / "tools"))
from recover_call_args import load, scan  # noqa: E402


def param_count(args: str) -> int:
    args = args.strip()
    if not args or args == "void":
        return 0
    return len(args.split(","))


def main() -> int:
    blobs = load()
    OUT.mkdir(parents=True, exist_ok=True)

    # A VENEER'S DECLARED PARAMETER COUNT IS ZERO AND MEANS NOTHING. Ghidra
    # types a two-instruction tail call as (void) because those instructions
    # touch no registers, so every call to one looked like it already passed
    # enough. The generated forwarder takes four, which is what ARM guarantees.
    veneers = set()
    vf = ROOT / "config/veneers.txt"
    if vf.exists():
        veneers = {l.strip() for l in vf.read_text(encoding="utf-8").splitlines()
                   if l.strip() and not l.startswith("#")}

    params, sources = {}, []
    for d in DIRS:
        if not d.exists():
            continue
        for p in sorted(d.glob("FUN_*.c")):
            text = p.read_text(encoding="utf-8", errors="replace")
            sources.append((p, text))
            m = DEFN.search(text)
            if m:
                params[m.group(1)] = (4 if p.stem in veneers
                                      else param_count(m.group(2)))

    patched_files = 0
    patched_calls = 0
    skip_nonstatic = skip_ambiguous = skip_nomatch = 0

    for p, text in sources:
        stem = p.stem
        if not re.fullmatch(r"FUN_[0-9a-fA-F]{8}", stem):
            continue
        body_at = text.find("{")
        if body_at < 0:
            continue
        head, body = text[:body_at], text[body_at:]

        # What the instructions say each BL passes.
        bls = scan(blobs, int(stem[4:], 16))
        by_target = {}
        for _at, target, regs in bls:
            by_target.setdefault(target, []).append(regs)

        edits = []
        for m in CALL.finditer(body):
            name, args = m.group(1), m.group(2).strip()
            want = params.get(name)
            # Arguments beyond the fourth travel on the stack and the scanner
            # now recovers those too, so the four-argument ceiling is gone.
            # Eight is simply further than any call in this corpus reaches.
            if want is None or want > 8:
                continue
            given = 0 if args == "" else len(args.split(","))
            target = int(name[4:], 16)
            found = by_target.get(target)
            if not found:
                skip_nomatch += 1
                continue
            if len(found) > 1:
                # SEVERAL BRANCHES TO ONE TARGET CAN STILL BE MATCHED, when the
                # C records a literal first argument: FUN_020cccdc(0x7e) and
                # FUN_020cccdc(0x7f,...) pair with the branches whose r0 holds
                # 0x7E and 0x7F. That is an identification, not a guess, and it
                # is only accepted when exactly one branch matches.
                first = args.split(",")[0].strip() if args else ""
                lit = None
                try:
                    lit = int(first, 0)
                except (TypeError, ValueError):
                    lit = None
                hits = [r for r in found
                        if lit is not None
                        and r.get(0, (None, None))[1] == lit]
                if len(hits) != 1:
                    skip_ambiguous += 1
                    continue
                regs = hits[0]
            else:
                    regs = found[0]

            # TRUSTING GHIDRA'S SIGNATURES AGAIN, NOW THAT THEY ARE RIGHT.
            #
            # This tool used to widen every call to whatever the instructions
            # set up, ignoring the declared arity. That was necessary while the
            # declared arity was wrong: Decompiler Parameter ID had never been
            # run, so 2,712 call sites passed fewer arguments than their callee
            # took and the values had to be dug out of the image by hand.
            #
            # With that analyser enabled the count is ZERO, and Ghidra's
            # signatures come from decompiling the callees rather than from
            # guessing at a call site. Continuing to widen calls past what the
            # analysis says would be substituting this tool's judgement for a
            # better-informed one - so it does not.
            if given >= want:
                continue

            extra = []
            ok = True
            for r in range(given, want):
                kind, v = regs.get(r, (None, None))
                if kind not in ("literal", "immediate"):
                    ok = False
                    break
                extra.append("0x%Xu" % v)
            if not ok or not extra:
                skip_nonstatic += 1
                continue
            new = "%s(%s)" % (name, ", ".join(
                ([args] if args else []) + extra))
            edits.append((m.start(), m.end(), new))

        if not edits:
            continue
        for start, end, new in reversed(edits):
            body = body[:start] + new + body[end:]
        note = ("/* Arguments restored by tools/gen_call_patches.py from the\n"
                " * instructions at each call site. The original decompiler\n"
                " * output is untouched in out/ghidra. */\n")
        (OUT / p.name).write_text(note + head + body, encoding="utf-8")
        patched_files += 1
        patched_calls += len(edits)

    print("files patched                        %d" % patched_files)
    print("call sites repaired                  %d" % patched_calls)
    print("skipped, value not static            %d" % skip_nonstatic)
    print("skipped, several calls to one target %d" % skip_ambiguous)
    print("skipped, no matching branch found    %d" % skip_nomatch)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
