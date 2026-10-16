"""Ask what this project already does about a function, before fixing it again.

WHY. This port has accumulated a lot of machinery - veneer forwarders, argument
patches, element-type overrides, host replacements, unfaithful wrappers, trace
wrappers, exclusions, transcriptions. That is good, and it has a cost: a
promising defect can turn out to be one somebody already fixed, and you only
find out after building.

Three times in one session:

  * FUN_020CCCDC "drops all four arguments at an indirect call" - it is in
    config/veneers.txt, and the generated forwarder passes r0-r2 and forces
    r3 = 0. It is the WORKED EXAMPLE in gen_veneers.py's own docstring.
  * a diagnostic that "proved" a stack corruption had never reached disk.
  * a structure dump too short to reach the fields it was taken as ruling out.

Only the first is this tool's business, but it is the one that repeats, because
the machinery grows and no single person remembers all of it. One command now
answers "what does this project already do about FUN_xxxxxxxx?"

USAGE
    python tools/whats_handled.py FUN_020cccdc
    python tools/whats_handled.py 020cccdc FUN_020d2374

Prints every config file that names it, whether a generated wrapper or
forwarder exists, and what that wrapper does - then says plainly whether the
function is already covered.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

CONFIGS = [
    ("config/veneers.txt",
     "a tail-call veneer; gen_veneers.py generates a forwarder that passes the "
     "arguments through and reproduces any register the veneer forces"),
    ("config/host_overrides.txt",
     "replaced by a host implementation in host_overrides.c"),
    ("config/unfaithful_wrapped.txt",
     "compiles, but reads storage Ghidra could not account for; wrapped so the "
     "run reports whether it was reached"),
    ("config/excluded_functions.txt",
     "deliberately not compiled - the body is unusable"),
    ("config/traced_functions.txt",
     "call-counted (NOTE: tracing perturbs this port)"),
    ("config/dat_element_types.txt",
     "has a DAT_ element-width override recorded against it"),
]

GENERATED = [
    ("recomp/decomp/src/veneers.c", "veneer forwarder"),
    ("recomp/decomp/src/trace_wrappers.c", "trace wrapper"),
    ("recomp/decomp/src/unfaithful_wrappers.c", "unfaithful wrapper"),
    ("recomp/decomp/src/host_overrides.c", "host implementation"),
]

PATCH_TOOLS = [
    ("tools/gen_vtable_patches.py", "an indirect-call argument transcription"),
    ("tools/gen_swi_patches.py", "a BIOS-call argument transcription"),
    ("tools/gen_call_patches.py", "a direct-call argument patch"),
    ("tools/gen_irq_patches.py", "an interrupt-restore argument patch"),
]


def show(name: str) -> None:
    print("=" * 70)
    print(name)
    print("=" * 70)
    found = False

    for rel, what in CONFIGS:
        p = ROOT / rel
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            if name in line and not line.lstrip().startswith("#"):
                print("  LISTED in %s" % rel)
                print("     -> %s" % what)
                found = True
                break

    for rel, what in GENERATED:
        p = ROOT / rel
        if not p.exists():
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"^.*\b%s\b.*$" % re.escape(name), text, re.M)
        if m:
            print("  HAS a generated %s in %s" % (what, rel))
            # Show the definition, which is usually the whole answer.
            i = text.find(m.group(0))
            body = text[i:i + 400].splitlines()[:6]
            for b in body:
                print("     | %s" % b)
            found = True

    for rel, what in PATCH_TOOLS:
        p = ROOT / rel
        if p.exists() and name in p.read_text(encoding="utf-8",
                                              errors="replace"):
            print("  TRANSCRIBED in %s" % rel)
            print("     -> %s" % what)
            found = True

    patched = ROOT / "out/work/patched" / (name + ".c")
    if patched.exists():
        print("  a PATCHED source exists at out/work/patched/%s.c" % name)
        print("     -> it is compiled from that, not from out/ghidra")
        found = True

    print()
    if found:
        print("  ALREADY COVERED in at least one way. Read the above before")
        print("  building a fix - it may be the fix.")
    else:
        print("  nothing in this project handles it specially.")
    print()


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    for a in args:
        a = a.lower()
        if not a.startswith("fun_"):
            a = "FUN_" + a
        else:
            a = "FUN_" + a[4:]
        show(a)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
