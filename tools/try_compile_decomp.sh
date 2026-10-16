#!/bin/sh
# Compile every decompiled body and report what the compiler accepts.
#
# This is the measurement the decomp-based recomp turns on: Ghidra's output is
# not C that anyone promised would compile, so the fraction that does - and the
# shape of what does not - decides how much work the approach costs.
#
# One gcc per file over 4,909 files is minutes of wall clock serially, so it
# runs parallel.
set -e
cd "$(dirname "$0")/.."
# ABSOLUTE COMPILER PATH, NOT "gcc". This machine has a Cygwin gcc first on
# PATH that cannot find its own stdbool.h, and a bare "gcc" inside an xargs
# subshell picked it up - 895 files "failed" on a missing standard header
# before the compiler itself was the suspect. Name the one we mean.
CC=/c/msys64/ucrt64/bin/gcc.exe
export PATH="/c/msys64/ucrt64/bin:$PATH"
OUT=out/work/decomp_build
mkdir -p "$OUT"
: > "$OUT/failures.txt"

compile_one() {
  f="$1"
  if /c/msys64/ucrt64/bin/gcc.exe -c -O0 -std=gnu11 -w \
        -I recomp/decomp/include -I recomp/decomp/gen \
        -include ghidra_types.h -include protos.h -include dat.h \
        "$f" -o /dev/null 2> "/tmp/$(basename "$f").err"; then
    echo "OK $f"
  else
    echo "NO $f"
    { echo "=== $f"; head -2 "/tmp/$(basename "$f").err"; } >> "$OUT/failures.txt"
  fi
  rm -f "/tmp/$(basename "$f").err"
}
export -f compile_one 2>/dev/null || true

ls out/ghidra/decomp/c/*.c out/ghidra/decomp_itcm/c/*.c 2>/dev/null \
  | xargs -P 8 -I{} sh -c '
      f="{}"
      if /c/msys64/ucrt64/bin/gcc.exe -c -O0 -std=gnu11 -w \
            -I recomp/decomp/include -I recomp/decomp/gen \
            -include ghidra_types.h -include protos.h -include dat.h \
            "$f" -o /dev/null 2>/dev/null; then echo "OK"; else echo "NO $f"; fi
    ' > "$OUT/results.txt"

ok=$(grep -c '^OK' "$OUT/results.txt" || true)
no=$(grep -c '^NO' "$OUT/results.txt" || true)
total=$((ok + no))
echo "compiled  $ok of $total"
echo "failed    $no"
grep '^NO' "$OUT/results.txt" | sed 's/^NO //' > "$OUT/failed_files.txt" || true
