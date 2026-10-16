#!/usr/bin/env bash
# Launch the port for a bounded test, capture both screens, and ALWAYS close it.
#
#   tools/testrun.sh [baseline|bountyhunter] [seconds] [outdir]
#
# WHY THIS EXISTS. Test launches were being left running: a stale instance from
# a previous build stayed on screen and had to be closed by hand. Whatever starts
# a program is responsible for stopping it, and "remember to kill it
# afterwards" is not a mechanism. This is.
#
# THREE THINGS IT GUARANTEES:
#
#   1. EXACTLY ONE INSTANCE. Any running port is killed first, so a forgotten
#      one from an earlier run cannot be mistaken for this one's behaviour -
#      which is exactly how a stale build got reported as a live bug.
#   2. NO STOLEN FOCUS. LSW2_NO_ACTIVATE=1 creates the windows without taking
#      the foreground, so a test run never interrupts whatever else is in
#      front on the machine.
#   3. IT CLOSES. A trap fires on normal exit, on error and on interrupt, so
#      even a failure partway through does not leave a window behind.
#
# Capture goes through the debug surface on TCP 19842, which interactive mode
# starts unconditionally (main.cpp: debug_pump_start before the frontend runs).
# So screenshots need no window focus either.

set -u
set -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-bountyhunter}"
SECONDS_TO_RUN="${2:-45}"
OUTDIR="${3:-$REPO/out/work/testrun}"

case "$MODE" in
    baseline)     EXE="$REPO/build/lsw2.exe" ;;
    bountyhunter) EXE="$REPO/build/lsw2_bountyhunter.exe" ;;
    *) echo "usage: $0 [baseline|bountyhunter] [seconds] [outdir]" >&2; exit 2 ;;
esac
[ -x "$EXE" ] || { echo "no executable at $EXE - build first" >&2; exit 1; }

mkdir -p "$OUTDIR"
LOG="$OUTDIR/$MODE.log"

kill_port() {
    # By image name, because the fork is a copy under a second name and both
    # must go. Redirected because "process not found" is the normal case.
    taskkill //IM lsw2.exe //F              >/dev/null 2>&1 || true
    taskkill //IM lsw2_bountyhunter.exe //F >/dev/null 2>&1 || true
}

cleanup() {
    local rc=$?
    kill_port
    echo "[testrun] closed"
    exit $rc
}
trap cleanup EXIT INT TERM

echo "[testrun] clearing any running instance"
kill_port
sleep 1

echo "[testrun] launching $MODE for ${SECONDS_TO_RUN}s (no focus steal)"
LSW2_NO_ACTIVATE=1 "$EXE" > "$LOG" 2>&1 &
GAME_PID=$!

# WAIT FOR THE SURFACE, NOT A FIXED SLEEP. Startup time varies with shader
# compilation, and a fixed sleep is either too short (capture fails) or too long
# (every test costs the worst case).
for _ in $(seq 1 60); do
    if python -c "import socket;socket.create_connection(('127.0.0.1',19842),timeout=1).close()" 2>/dev/null; then
        break
    fi
    # If the process died, stop waiting for a socket that will never open.
    kill -0 "$GAME_PID" 2>/dev/null || { echo "[testrun] the port exited early:"; tail -20 "$LOG"; exit 1; }
    sleep 1
done

echo "[testrun] up; letting it run"
sleep "$SECONDS_TO_RUN"

echo "[testrun] capturing"
python "$REPO/tools/pilot.py" shot "$OUTDIR/$MODE.png" 2>&1 | sed 's/^/  /' || true

echo "[testrun] log:"
grep -iE "fork\]|settings\]|error|fail|warn" "$LOG" | head -20 | sed 's/^/  /' || true
