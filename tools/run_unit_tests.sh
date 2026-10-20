#!/usr/bin/env bash
# Build and run every unit test in the port, and say honestly which ones ran.
#
#   tools/run_unit_tests.sh              everything, master-server tests skipped
#   tools/run_unit_tests.sh --with-server  start the directory first, then stop it
#
# WHY THIS EXISTS. The source lists are not guessable. `test_tcp.cpp` exercises
# the transport THROUGH a lockstep session, so it needs lsw2_lockstep.cpp on the
# link line even though it is not a lockstep test - and getting that wrong
# produces a page of undefined references that reads like a broken test rather
# than a wrong command. That happened twice before this file existed.
#
# THREE OUTCOMES, NOT TWO. A test that cannot reach the master server exits 2,
# and that is not a failure - it is a test declining to pretend. Collapsing it
# into "fail" would train everyone to ignore reds; collapsing it into "pass"
# would be a lie. It gets its own column.

set -u
set -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

UCRT="${UCRT64_BIN:-/c/msys64/ucrt64/bin}"
export PATH="$UCRT:/c/msys64/usr/bin:$PATH"

OUT="$REPO/out/work/unit"
mkdir -p "$OUT"

WITH_SERVER=0
[ "${1:-}" = "--with-server" ] && WITH_SERVER=1

SERVER_PID=""
stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null && echo "" && echo "stopped the master server (pid $SERVER_PID)"
        SERVER_PID=""
    fi
}
# ALWAYS, including on error and on interrupt. Leaving a listener bound to
# 28553 makes the next run's "cannot reach" message a lie in the other
# direction - it would reach a server nobody meant to still be running.
trap stop_server EXIT INT TERM

if [ $WITH_SERVER -eq 1 ]; then
    # Every piece of runtime state goes under $OUT. The defaults are relative to
    # the working directory, so without this a test run leaves a database, a
    # bans.txt and a logs/ directory sitting in the repository root - which is
    # how master.sqlite3 came to be a tracked file in the first place.
    python services/master-server-selfhost/master_server.py --port 28553 \
        --db "$OUT/master.sqlite3" --bans "$OUT/master-bans.txt" \
        --logs "$OUT/master-logs" \
        > "$OUT/master.log" 2>&1 &
    SERVER_PID=$!
    sleep 2
    echo "master server up on 127.0.0.1:28553 (pid $SERVER_PID)"
    echo ""
fi

# name : sources
TESTS=(
    "gamemenu      : port/frontend/test_gamemenu.cpp port/frontend/gamemenu.cpp"
    "gamefont      : port/frontend/test_gamefont.cpp port/frontend/gamefont.cpp"
    "strings       : port/frontend/test_strings.cpp port/frontend/strings.cpp"
    "scriptqueue   : port/frontend/test_scriptqueue.cpp port/frontend/scriptqueue.cpp"
    "forkpopup     : port/frontend/test_forkpopup.cpp port/frontend/forkpopup.cpp"
    "customization : port/frontend/test_customization.cpp port/frontend/customization_policy.cpp"
    "lasthunter    : port/frontend/test_lasthunter.cpp port/frontend/lasthunter.cpp"
    "characterpick : port/frontend/test_characterpick.cpp port/frontend/characterpick.cpp"
    "roster        : port/frontend/test_roster.cpp port/frontend/roster.cpp port/frontend/characterpick.cpp port/frontend/lasthunter.cpp port/frontend/hostsettings.cpp port/frontend/gamemenu.cpp"
    "objsprite     : port/frontend/test_objsprite.cpp port/frontend/objsprite.cpp"
    "screenengine  : port/frontend/test_screenengine.cpp port/frontend/screenengine_policy.cpp"
    "joinmenu      : port/frontend/test_joinmenu.cpp port/frontend/joinmenu.cpp port/frontend/gamemenu.cpp"
    "masterlist    : port/frontend/test_masterlist.cpp port/frontend/masterlist.cpp port/frontend/gamemenu.cpp port/net/lsw2_master.cpp"
    "scorescreen   : port/frontend/test_scorescreen.cpp port/frontend/scorescreen_policy.cpp"
    "lobbytimer    : port/frontend/test_lobbytimer.cpp port/frontend/lobbytimer.cpp"
    "enhanced      : port/frontend/test_enhanced.cpp port/frontend/enhanced_policy.cpp"
    "extrasapply   : port/frontend/test_extrasapply.cpp port/frontend/extrasapply_policy.cpp"
    "selectskip    : port/frontend/test_selectskip.cpp port/frontend/selectskip_policy.cpp"
    "lobbypin      : port/frontend/test_lobbypin.cpp port/frontend/lobbypin.cpp"
    "pauselayout   : port/frontend/test_pauselayout.cpp port/frontend/pauselayout.cpp"
    "inputlog      : port/frontend/test_inputlog.cpp port/frontend/inputlog.cpp"
    "partyportrait : port/frontend/test_partyportrait.cpp port/frontend/partyportrait_policy.cpp"
    "hudportrait   : port/frontend/test_hudportrait.cpp port/frontend/hudportrait_policy.cpp"
    "sessionpause  : port/frontend/test_sessionpause.cpp port/frontend/sessionpause_policy.cpp"
    "dsname        : port/frontend/test_dsname.cpp port/frontend/dsname_policy.cpp"
    "cartroster    : port/frontend/test_cartroster.cpp port/frontend/cartroster_policy.cpp"
    "slotcolour    : port/frontend/test_slotcolour.cpp port/frontend/slotcolour_policy.cpp"
    "viewmask      : port/frontend/test_viewmask.cpp port/frontend/viewmask_policy.cpp"
    "partslot      : port/frontend/test_partslot.cpp port/frontend/partslot_policy.cpp"
    "netmsglog     : port/frontend/test_netmsglog.cpp port/frontend/netmsglog_policy.cpp"
    "coophosts     : port/frontend/test_coophosts.cpp port/frontend/coophosts.cpp"
    "hostsettings  : port/frontend/test_hostsettings.cpp port/frontend/hostsettings.cpp port/frontend/gamemenu.cpp port/frontend/characterpick.cpp"
    "consolemenu   : port/frontend/test_consolemenu.cpp port/frontend/consolemenu.cpp port/frontend/gamemenu.cpp"
    "name          : port/net/test_name.cpp port/net/lsw2_name.cpp"
    "lan           : port/net/test_lan.cpp port/net/lsw2_lan.cpp port/net/lsw2_name.cpp port/net/lsw2_master.cpp"
    "lockstep      : port/net/test_lockstep.cpp port/net/lsw2_lockstep.cpp port/net/lsw2_tcp.cpp"
    "tcp           : port/net/test_tcp.cpp port/net/lsw2_tcp.cpp port/net/lsw2_lockstep.cpp"
    "vwh           : port/net/test_vwh.cpp port/net/lsw2_vwh.cpp port/net/lsw2_tcp.cpp"
    "master_client : port/net/test_master_client.cpp port/net/lsw2_master.cpp"
    "master_timeout: port/net/test_master_timeout.cpp port/net/lsw2_master.cpp"
    "ping          : port/net/test_ping.cpp port/net/lsw2_ping.cpp"
    "wireless_relay: port/net/test_wireless_relay.cpp port/net/lsw2_wireless_relay.cpp"
    "relay_session : port/net/test_relay_session.cpp port/net/lsw2_relay_session.cpp port/net/lsw2_wireless_relay.cpp"
    "upnp          : port/net/test_upnp.cpp port/net/lsw2_upnp_parse.cpp"
    "wsrelay       : port/net/test_wsrelay.cpp port/net/lsw2_wsrelay.cpp"
    "forksession   : port/frontend/test_forksession.cpp port/frontend/forksession.cpp port/frontend/lobbytimer.cpp port/frontend/characterpick.cpp port/frontend/roster.cpp port/frontend/test_rosterstub.cpp port/frontend/lasthunter.cpp port/frontend/consolemenu.cpp port/frontend/hostsettings.cpp port/frontend/joinmenu.cpp port/frontend/masterlist.cpp port/frontend/gamemenu.cpp port/frontend/menuhost.cpp port/frontend/forkpopup.cpp port/net/lsw2_lan.cpp port/net/lsw2_lockstep.cpp port/net/lsw2_master.cpp port/net/lsw2_name.cpp port/net/lsw2_net_bind.cpp port/net/lsw2_ping.cpp port/net/lsw2_tcp.cpp port/net/lsw2_vwh.cpp port/net/lsw2_upnp.cpp port/net/lsw2_upnp_parse.cpp port/net/lsw2_relay_session.cpp port/net/lsw2_wireless_relay.cpp port/net/lsw2_wsrelay.cpp"
)

pass=0; fail=0; skip=0; failed_names=""

for entry in "${TESTS[@]}"; do
    name="${entry%%:*}"; name="${name// /}"
    srcs="${entry#*:}"
    printf '  %-14s ' "$name"

    if ! g++ -O1 -std=c++20 -Wall -Wextra -o "$OUT/$name.exe" $srcs -lws2_32 \
            > "$OUT/$name.build.log" 2>&1; then
        echo "BUILD FAILED  (see out/work/unit/$name.build.log)"
        fail=$((fail + 1)); failed_names="$failed_names $name"
        continue
    fi

    "$OUT/$name.exe" > "$OUT/$name.run.log" 2>&1
    case $? in
        0) # `grep -c` exits 1 when it counts zero, so the count and the exit
           # code disagree about whether anything went wrong. Take the number
           # and ignore the status - a test that prints no "ok" lines at all
           # (test_scriptqueue only reports at the end) still passed.
           n=$(grep -cE '^[[:space:]]+ok' "$OUT/$name.run.log" 2>/dev/null) || true
           [ -z "$n" ] && n=0
           if [ "$n" -gt 0 ]; then echo "pass    $n checks"; else echo "pass"; fi
           pass=$((pass + 1)) ;;
        2) echo "skipped (needs the master server; pass --with-server)"
           skip=$((skip + 1)) ;;
        *) echo "FAILED  (see out/work/unit/$name.run.log)"
           fail=$((fail + 1)); failed_names="$failed_names $name" ;;
    esac
done

# The service half. These need no compiler and start their own listener on a
# free port, so they run unconditionally - --with-server is about the C++ client
# tests that dial a directory, not these.
PY_TESTS=(
    "master_server: services/master-server-selfhost/test_master_server.py"
    "master_gui   : services/master-server-selfhost/test_master_gui.py"
    # The ROM-reading primitives. Skips (exit 2) without an extracted image,
    # which is the honest outcome for a checkout that has not run extraction.
    "armscan      : tools/armscan.py"
    # Every override REPLACES. This asks who else called the function, and
    # fails on a NEW override or a moved count - the check that would have
    # saved three passes on the pause menu's co-op lobby entry.
    "override_targets: tools/sweep_override_targets.py"
    "override_reachable: tools/sweep_override_reachable.py"
    "guest_addresses: tools/sweep_guest_addresses.py"
    "build_gating: tools/sweep_build_gating.py"
    "log_reachable: tools/sweep_log_reachable.py"
    # A caller that is a TEST is not a caller. on_menu_event, the game
    # font and the LAN browser's expiry were each written, tested and
    # never reached; a green suite cannot see this because the suite is
    # the thing doing the calling.
    "test_only   : tools/sweep_test_only.py"
    # A value that is STORED and never read. g_last held every menu
    # event the player generated and nothing ever looked at it.
    "stored_unread: tools/sweep_stored_unread.py"
    # Every setting must have something that ACTS on it. Unregistered
    # until now because it reported 21 correctly-wired settings; the
    # four causes were its own and are fixed.
    "setting_consumers: tools/sweep_setting_consumers.py"
    # The same question one level up: a STRUCT OR CLASS MEMBER written
    # and never read. sweep_stored_unread sees file statics only.
    "member_unread: tools/sweep_member_unread.py"
    # A two-value setting can pass setting_consumers with one position
    # dead - the FIELD is read, so that sweep is satisfied.
    "enum_handled: tools/sweep_enum_handled.py"
    # A fork screen that can be OPENED but has no on_menu_event branch
    # draws its rows and drops every press - the on_menu_event bug one
    # size down.
    "screen_handlers: tools/sweep_screen_handlers.py"
    # A comment saying "off by default" over a default of true. Stale
    # comments have cost five corrections in two days; this is the one
    # slice of that which is mechanical.
    "default_claims: tools/sweep_default_claims.py"
    # The runbook is read during a play session, where it cannot be checked
    # against the source. This checks it in advance.
    "runbook      : tools/sweep_runbook.py"
    # Port constants transcribed from the ROM, checked against the exports the
    # ROM is read into. The gap a hat-membership constant fell through: right when written,
    # silently wrong after better evidence arrived.
    "rom_constants: tools/sweep_rom_constants.py"
    # One guest fact declared under two names, which no test can see: a
    # correction to one copy leaves the other silently disagreeing. Registered
    # only after it ran RED for five commits and every real duplicate it found
    # was collapsed - twenty groups down to a baseline of coincidences.
    "dup_constants: tools/sweep_duplicate_constants.py"
    # ui_fork_build() is false at registration time in BOTH executables, so a
    # guard written on it there is inert. That mistake has been made twice in
    # one function - the fork's menu strings, then the baseline's co-op
    # override - and the second time the explanation was already sitting forty
    # lines above it.
    "startup_pred : tools/sweep_startup_predicate.py"
    # Every other gate here reads SOURCE. This one reads the BINARY: are the
    # required feature symbols in build/lsw2.exe, is that exe newer than the source,
    # and is the fork exe still byte-identical to it. The shipped build was a
    # day and thirty commits stale while everything else reported green.
    "exe_reachable: tools/sweep_exe_reachable.py"
    # Being CALLED is not being REACHED. sweep_features catches a function whose
    # last caller went; this catches the one whose caller is itself dead, which
    # every other gate here passes.
    "call_reachable: tools/sweep_call_reachable.py"
)

echo ""
for entry in "${PY_TESTS[@]}"; do
    name="${entry%%:*}"; name="${name// /}"
    src="${entry#*:}"; src="${src// /}"
    printf '  %-14s ' "$name"

    if ! command -v python > /dev/null 2>&1; then
        echo "skipped (no python)"; skip=$((skip + 1)); continue
    fi
    # A check that is not part of this checkout (the public tree leaves out
    # the ones that read private material) is skipped, and says so.
    if [ ! -f "$src" ]; then
        echo "skipped (not in this checkout)"; skip=$((skip + 1)); continue
    fi

    # Run from the service's own directory: it imports its siblings by plain
    # module name, the way both executables do.
    ( cd "$(dirname "$src")" && python "$(basename "$src")" ) \
        > "$OUT/$name.run.log" 2>&1
    case $? in
        0) n=$(grep -cE '^[[:space:]]+ok' "$OUT/$name.run.log" 2>/dev/null) || true
           [ -z "$n" ] && n=0
           if [ "$n" -gt 0 ]; then echo "pass    $n checks"; else echo "pass"; fi
           pass=$((pass + 1)) ;;
        2) echo "skipped ($(tail -n 1 "$OUT/$name.run.log"))"
           skip=$((skip + 1)) ;;
        *) echo "FAILED  (see out/work/unit/$name.run.log)"
           fail=$((fail + 1)); failed_names="$failed_names $name" ;;
    esac
done

# THE SOURCE SWEEPS, RUN AS GATES RATHER THAN AS REPORTS.
#
# Both check things a compiler cannot: that every function declared in a port
# header is actually reached, and that every line LSW2_DIAG prints is explained
# in the legend it prints first. Both defects have shipped repeatedly - six
# orphans, and `netplayers` (the byte that gates the cartridge's whole message
# layer) printed with no legend entry - and both are invisible to a green build.
#
# Counted into the same pass/fail tally, because a check that only prints is a
# check nobody reads.
#
# sweep_braces joined them after a braceless `if` shipped: a
# second call added under a one-statement body ran unconditionally while the
# indentation said it did not. Green build, sixty-one green gates, wrong code.
for sweep in sweep_orphans sweep_braces sweep_diag_legend sweep_lr_checks \
             sweep_features; do
    if out=$(python "$(dirname "$0")/$sweep.py" 2>&1); then
        printf '  %-14s pass    %s
' "$sweep" "$(printf '%s' "$out" | tail -1)"
        pass=$((pass + 1))
    else
        printf '  %-14s FAILED
' "$sweep"
        printf '%s
' "$out" | sed 's/^/      /'
        fail=$((fail + 1)); failed_names="$failed_names $sweep"
    fi
done

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ -n "$failed_names" ] && echo "failed:$failed_names"
[ $fail -eq 0 ]
