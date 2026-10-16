"""Does the required feature code actually reach the executable people run?

WHY THIS EXISTS, AND WHY IT IS DIFFERENT FROM EVERY OTHER GATE HERE.
`sweep_features.py` asks whether a symbol is defined and called IN THE SOURCE.
That is a real question and it has caught real bugs, but it is answered by
reading text. It cannot tell you:

  * whether the file is in the build at all;
  * whether the build currently SUCCEEDS;
  * whether the linker kept the symbol;
  * whether the executable sitting in build/ is older than the work.

That last one is not hypothetical. The shipped `build/lsw2.exe` was once a
day old, thirty commits behind the tree,
while every gate in the repo reported green. Green gates are not a green
build, and a green build is not a SHIPPED build.

    python tools/sweep_exe_reachable.py

WHAT IT CHECKS. Every symbol `sweep_features.CLAUSES` names, against the symbol
table of `build/lsw2.exe`, plus:

  * the executable is NEWER than every source file it was built from, so a
    stale binary fails instead of passing on yesterday's symbols;
  * `build/lsw2_bountyhunter.exe` is byte-identical to it, which is how
    port/build.sh produces the fork - the same binary under a second name,
    picking its save from argv[0]. If they ever diverge, "it works in the
    fork" and "it works in the baseline" stop being the same statement and
    this gate should be the thing that notices.

THE SYMBOLS ARE MANGLED, which cost me a wrong answer before it cost anything
else: `nm` prints `_Z24lsw2_arena_hud_compositePKji`, so a search for
`lsw2_arena_hud_composite$` finds nothing and every clause looks absent. The
match is therefore a substring of the mangled name, which is loose in the way
that matters least - a false PASS needs a different symbol to contain this
one's whole name.

WHAT IT STILL CANNOT DO. Tell you the code RUNS. A linked symbol that nothing
calls at runtime is exactly the shape `sweep_test_only.py` and
`sweep_build_gating.py` exist for, and this gate is deliberately the crude
outer layer: it answers "is it in the box", not "does the box use it".
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "lsw2.exe"
FORK = ROOT / "build" / "lsw2_bountyhunter.exe"
SOURCES = [ROOT / "port" / "frontend", ROOT / "port" / "net"]


def symbol_blob() -> str | None:
    nm = shutil.which("nm")
    if not nm:
        return None
    try:
        out = subprocess.run([nm, str(EXE)], capture_output=True, text=True,
                             timeout=300)
    except Exception:
        return None
    return out.stdout if out.stdout else None


def newest_source() -> tuple[float, Path | None]:
    newest, where = 0.0, None
    for d in SOURCES:
        if not d.is_dir():
            continue
        for p in list(d.glob("*.cpp")) + list(d.glob("*.h")):
            if p.name.startswith("test_"):
                continue
            m = p.stat().st_mtime
            if m > newest:
                newest, where = m, p
    return newest, where


def main() -> int:
    # NOT BUILT is a skip (exit 2), not a failure: there is no binary to be
    # stale, the same way armscan skips a checkout with no extracted image. A
    # fresh clone and CI (which has no ROM, so can never build) land here.
    if not EXE.is_file():
        print(f"no {EXE.relative_to(ROOT).as_posix()} - run port/build.sh")
        return 2

    bad = []

    # A BINARY OLDER THAN THE SOURCE IS A BINARY WITHOUT THE WORK IN IT.
    exe_time = EXE.stat().st_mtime
    src_time, src_path = newest_source()
    if src_path is not None and src_time > exe_time:
        bad.append(f"{src_path.relative_to(ROOT).as_posix()} is NEWER than "
                   f"build/lsw2.exe - the executable does not contain it; "
                   f"run port/build.sh")

    # THE FORK IS THE SAME BINARY, and that is load-bearing rather than
    # incidental: it is why "reachable in the baseline" and "reachable in the
    # fork" are one question today.
    if not FORK.is_file():
        bad.append("build/lsw2_bountyhunter.exe is missing - port/build.sh "
                   "copies it from lsw2.exe")
    elif FORK.read_bytes() != EXE.read_bytes():
        bad.append("build/lsw2_bountyhunter.exe differs from build/lsw2.exe. "
                   "They are one binary under two names today, so this gate's "
                   "single-symbol-table answer would no longer cover both - "
                   "check each separately before trusting it")

    sys.path.insert(0, str(ROOT / "tools"))
    import sweep_features  # noqa: E402

    blob = symbol_blob()
    if blob is None:
        print("nm is unavailable or produced nothing - cannot read the "
              "executable's symbols.")
        print("Put msys2 on PATH: export "
              "PATH=\"/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH\"")
        return 1

    wanted = sorted({sym for _, _, sym in sweep_features.CLAUSES})
    missing = [s for s in wanted if s not in blob]
    for s in missing:
        points = sorted({p for p, _, sym in sweep_features.CLAUSES if sym == s})
        bad.append(f"{s} is in no symbol of build/lsw2.exe "
                   f"(feature {', '.join(points)})")

    if bad:
        print("the required feature code is not all in the executable:")
        for line in bad:
            print(f"  {line}")
        print("\n  sweep_features reads SOURCE. This reads the BINARY, and the"
              "\n  two disagreeing is the case that ships.")
        return 1

    by_point: dict[str, int] = {}
    for point, _, _ in sweep_features.CLAUSES:
        by_point[point] = by_point.get(point, 0) + 1
    summary = "  ".join(f"{k} x{v}" for k, v in sorted(by_point.items()))
    print(f"all {len(wanted)} feature symbols are in build/lsw2.exe, which is "
          f"newer than every source")
    print(f"  and lsw2_bountyhunter.exe is byte-identical to it")
    print(f"  {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
