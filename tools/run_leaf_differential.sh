#!/bin/sh
# Run the leaf differential to completion, restarting past anything that kills
# the process.
#
# A decompiled function given inputs it never sees in the real game can write
# through a wild pointer and take the process down. The harness writes a BEGIN
# line before each function and a RESULT line after, both flushed, so a death
# leaves exactly one BEGIN with no RESULT - the function that caused it. This
# records that function as a fault and resumes at the next one, so one bad
# function costs one restart rather than the whole run.
set -e
cd "$(dirname "$0")/.."

EXE=out/work/closure_build/test_diff.exe
LOG=out/work/closure_build/leaves.log
TOTAL=$(grep -ac '^    {0x' recomp/decomp/gen/leaf_table.h)

# A timed-out harness can still be holding the log when the next run starts,
# and on Windows that makes the file un-removable rather than silently
# overwritten. Clear any stragglers first so a stale process cannot make the
# whole sweep look like a failure.
taskkill //F //IM test_diff.exe >/dev/null 2>&1 || true
rm -f "$LOG" || { echo "the log is still held - is a previous run alive?"; exit 1; }
next=0
restarts=0
while [ "$next" -lt "$TOTAL" ]; do
    # A PER-PROCESS TIMEOUT, because a wild pointer is not the only way a
    # decompiled function misbehaves on inputs it never sees in the real game -
    # it can also loop forever. The interpreter has a cycle budget and stops;
    # the native side has nothing, so the timeout is the only bound. 20 seconds
    # is far longer than any leaf needs and short enough to sweep 544 of them.
    timeout 20 "$EXE" . "$next" "$LOG" >/dev/null 2>&1 || true
    if grep -aq '^DONE' "$LOG"; then
        break
    fi
    last_begin=$(grep -a '^BEGIN ' "$LOG" | tail -1 | cut -d' ' -f2)
    last_result=$(grep -a '^RESULT ' "$LOG" | tail -1 | cut -d' ' -f2)
    if [ -z "$last_begin" ]; then
        echo "the harness produced nothing - stopping"
        exit 1
    fi
    if [ "$last_begin" = "$last_result" ]; then
        # It finished that one and died between functions.
        next=$((last_begin + 1))
    else
        name=$(grep -a "^BEGIN $last_begin " "$LOG" | tail -1 | cut -d' ' -f3)
        echo "RESULT $last_begin $name fault arm" >> "$LOG"
        echo "  the decompiled C faulted or ran away" >> "$LOG"
        next=$((last_begin + 1))
    fi
    restarts=$((restarts + 1))
    if [ "$restarts" -gt 600 ]; then
        echo "too many restarts - stopping at $next of $TOTAL"
        break
    fi
done

echo "leaf functions          $TOTAL"
echo "  agreed                $(grep -ac ' agree ' "$LOG" || true)"
echo "  DISAGREED             $(grep -ac ' disagree ' "$LOG" || true)"
echo "  faulted the process   $(grep -ac ' fault ' "$LOG" || true)"
echo "  skipped (no reference)$(grep -ac ' skip ' "$LOG" || true)"
echo "  restarts              $restarts"
echo "  thumb functions found $(grep -a ' thumb$' "$LOG" | wc -l)"
