#!/usr/bin/env python3
"""Post-lift patch: guard the netgame tick's transient state-38 request.

FUN_020b07e8 requests state 38 via `bl 0x020b04f0` at guest 0x020B08E8 (return address
0x020B08EC) when the player count churns to 0 during a 2nd child's join. This wraps that
ONE call site with `if (!lsw2_fork_suppress_net38())` so the fork can suppress the
transient 38 on the host (LSW2_TICK_TOLERATE). Identified uniquely by the return-address
0x020B08ECu (the push_return immediately before the call), so the other FUN_020b04f0
call sites are untouched.

Usage: python3 postlift_net38.py <stage_dir>   (runs on every generated shard; idempotent)
"""
import sys, glob, os

MARKER = "runtime_call_push_return(0x020B08ECu);"
CALL = "lsw2_arm9_FUN_020b04f0();"
GUARDED = "if (!lsw2_fork_suppress_net38()) lsw2_arm9_FUN_020b04f0();"
# A WEAK default (returns 0 = no suppression) so the framework runner exe - which links
# libnds_banks.a WITHOUT the fork - resolves the symbol; the fork's strong extern "C"
# definition in the shell overrides it. Without this the runner link fails (undefined ref).
DECL = "__attribute__((weak)) int lsw2_fork_suppress_net38(void){return 0;}"

def patch_file(path):
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    if MARKER not in text:
        return False
    if "lsw2_fork_suppress_net38" in text:
        return "already"   # idempotent
    lines = text.split("\n")
    # find the MARKER, then the next bare CALL line after it, and guard that line only
    mi = next(i for i, ln in enumerate(lines) if MARKER in ln)
    ci = next(i for i in range(mi, min(mi + 12, len(lines))) if lines[i].strip() == CALL)
    indent = lines[ci][:len(lines[ci]) - len(lines[ci].lstrip())]
    lines[ci] = indent + GUARDED
    # inject the extern decl after the first #include (or at top)
    inc = next((i for i, ln in enumerate(lines) if ln.startswith("#include")), 0)
    lines.insert(inc + 1, DECL)
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("\n".join(lines))
    return True

def main():
    stage = sys.argv[1]
    patched = skipped = 0
    for path in glob.glob(os.path.join(stage, "lsw2_arm9_*.c")):
        r = patch_file(path)
        if r == "already":
            skipped += 1
        elif r:
            patched += 1
            print("   [postlift] net38-tolerate patched %s" % os.path.basename(path))
    if patched == 0 and skipped == 0:
        print("   [postlift] WARNING: net38 marker not found in any shard - patch NOT applied")
        return 1
    if patched == 0:
        print("   [postlift] net38-tolerate already applied")
    return 0

if __name__ == "__main__":
    sys.exit(main())
