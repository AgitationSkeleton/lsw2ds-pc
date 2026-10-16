#!/bin/sh
# Compile the Bounty Hunter closure from Ghidra's decompiled C.
#
# THE -D FLAG IS THE LOAD-BEARING PART. Every function is declared
# argument-unspecified in protos.h so that a call site and a definition cannot
# contradict each other (see tools/gen_protos_permissive.py). A file must not
# see that declaration for the function IT defines, or the permissive
# declaration and the real definition conflict - so the build defines the
# guard for the file's own function, and only that one.
set -e
cd "$(dirname "$0")/.."
export PATH="/c/msys64/ucrt64/bin:$PATH"
OUT=out/work/closure_build
mkdir -p "$OUT" "$OUT/obj"

if [ ! -s "$OUT/files.txt" ]; then
python - <<'PY' > "$OUT/files.txt"
import json, pathlib
inv = json.load(open('config/recomp_inventory.json', encoding='utf-8'))
dirs = [pathlib.Path('out/ghidra/decomp/c'), pathlib.Path('out/ghidra/decomp_itcm/c')]
for f in inv['functions']:
    for d in dirs:
        p = d / ('FUN_%s.c' % f['address'][2:])
        if p.exists():
            print(p); break
PY
fi

: > "$OUT/results.txt"
xargs -a "$OUT/files.txt" -P 8 -I{} sh -c '
  f="{}"; base=$(basename "$f" .c)
  # A function the host replaces is not compiled from the decomp at all, so
  # the two definitions cannot both reach the link.
  if grep -qx "$base" config/excluded_functions.txt 2>/dev/null; then
    rm -f out/work/closure_build/obj/"$base".o; echo EXCLUDED; exit 0; fi
  if grep -qx "$base" config/host_overrides.txt 2>/dev/null; then
    rm -f out/work/closure_build/obj/"$base".o; echo OVERRIDE; exit 0; fi
  # A function that reads unaccounted registers keeps its body but loses its
  # name: it compiles under FUN_x_unfaithful, and a generated wrapper takes the
  # real name so every call is recorded. Nothing in the decompiled source
  # changes - see tools/gen_unfaithful_wrappers.py.
  rename=""
  if grep -qx "$base" config/unfaithful_wrapped.txt 2>/dev/null; then
    rename="-D${base}=${base}_unfaithful"
  fi
  guards=$(grep -a "^$base " out/work/selfguards.txt | cut -d" " -f2-)
  if /c/msys64/ucrt64/bin/gcc.exe -c -O1 -std=gnu11 -w -fno-strict-aliasing -fpermissive \
       $guards $rename \
       -I recomp/decomp/include -I recomp/decomp/gen \
       -include ghidra_types.h -include ghidra_intrinsics.h \
       -include protos.h -include dat/"$base".h \
       "$f" -o out/work/closure_build/obj/"$base".o 2>/dev/null
  then echo "OK"; else echo "NO $f"; fi
' >> "$OUT/results.txt"

ov=$(grep -ac '^OVERRIDE' "$OUT/results.txt" || true)
ok=$(grep -ac '^OK' "$OUT/results.txt" || true)
no=$(grep -ac '^NO' "$OUT/results.txt" || true)
grep -a '^NO ' "$OUT/results.txt" | awk '{print $2}' > "$OUT/failed.txt" || true
echo "closure functions : $((ok + no))"
echo "compiled          : $ok"
echo "failed            : $no"
echo "host-replaced     : $ov"
echo "excluded          : $(grep -ac '^EXCLUDED' "$OUT/results.txt" || true)"
