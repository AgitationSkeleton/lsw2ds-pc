#!/usr/bin/env bash
# LSW2 PC port - one launcher, both builds, shared settings and assets.
#
#   port/launch.sh                       baseline (the game as shipped)
#   port/launch.sh --mode bountyhunter   the Bounty Hunter fork
#   port/launch.sh --extract-assets      unpack the ROM for modding
#   port/launch.sh --fresh               delete this mode's save first
#
# OUT OF THE BOX IT NEEDS ONLY A ROM. Drop a valid LEGO Star Wars II (USA)
# cartridge dump in rom/ and run this. No BIOS dumps, no firmware, no manual
# extraction step - the same promise Ship of Harkinian makes for Ocarina.
#
# That works because the runner embeds FreeBIOS (Gilead Kutnick, BSD-2-Clause)
# and can synthesize a firmware image, so `--freebios --generated-firmware
# --boot direct` needs nothing beyond the ROM. VERIFIED, not assumed: on the same
# 400M-cycle boot path, FreeBIOS reaches the identical ARM9 PC (0x020CEA8C) as
# retail dumps, with dispatch counts within about 1%.
#
# IF YOU HAVE YOUR OWN DUMPS they are used automatically when present in
# port/bios/ - that is the framework's oracle-diffed path and is strictly
# preferred. It is an upgrade, never a requirement.
#
# BASELINE VS FORK. Both builds share this directory, these settings and these
# assets. They differ in save file and in content: the baseline runs the game as
# shipped - no unlocks, no stud changes - because its whole value is being the
# reference the fork is measured against. The fork seeds an unlocked save and
# owns roster and host-option changes.

set -u
set -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="$REPO/port"
# Our shell by default; --framework-frontend falls back to the framework's.
# Set once the mode is known - see "which executable" below.
EXE=""
FALLBACK_EXE="$REPO/out/work/port-build/runner/nds_runner.exe"
ROM_SHA1="7c1afa070797945ba5a2577c7ddfded5cf2fc443"

export PATH="${UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH"   # SDL2.dll lives here

die() { echo "" >&2; echo "error: $*" >&2; exit 1; }

MODE=baseline
FRESH=0
EXTRACT=0

# WHICH COPY OF THE GAME THIS IS, for testing the networking on one machine.
#
# Two fork instances collide three ways and all three are silent-ish: the same
# save file, the same settings.json - so the same player name AND the same
# listen port - and therefore a second host that cannot bind. The only visible
# symptom is "could not listen - is the port already in use?", which reads as a
# firewall problem rather than as two copies fighting.
#
# --instance 2 gives the second copy its own save, its own settings file and its
# own ports. Instance 1 is unchanged, so the shared-settings rule below still
# holds for every ordinary launch.
#
# TWO PORTS, and only one of them used to move. The settings file got its own
# `mp_listen_port` - the game's own multiplayer socket - while both copies
# still asked the runner for DEBUG surface 19842, so the second one silently
# had none and tools/pilot.py could not reach it. The exec below now derives it
# the same way: instance N serves 19842 + N - 1.
INSTANCE=1
# INTERNET PLAY over the cartridge's own radio. The radio is loopback
# UDP; the fork carries it across a network with an in-process relay
# (lsw2_relay_session). Which end this is, and where to reach the peer, are
# launch-time facts - the runner's wireless base/index and the WH role are set
# before boot - so this is a launcher flag, not an in-game button.
#   port/launch.sh --mode bountyhunter --host                 (host a game)
#   port/launch.sh --mode bountyhunter --join <host-ip>       (join one)
# Proven topology (the relay_admit test): host is wireless instance 0, joiner
# instance 1, both on the same base; the relay catches base+peer and injects
# base+self, and the TCP link carries it. The host must have --link-port
# reachable (port-forward or UPnP it); default 55000.
NET_ROLE=""
NET_PEER=""
WBASE=30000
LINK_PORT=55000
PASSTHROUGH=()
while [ $# -gt 0 ]; do
    # Value-taking options shift once for the flag and once more only if a value
    # is actually present - `shift 2` on a trailing flag fails under set -u and
    # loops forever (a real bug hit by `--join` with no IP).
    case "$1" in
        --mode) MODE="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
        --instance) INSTANCE="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
        --fresh) FRESH=1; shift ;;
        --extract-assets) EXTRACT=1; shift ;;
        --host) NET_ROLE=host; shift ;;
        --join) NET_ROLE=join; NET_PEER="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
        --wireless-base) WBASE="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
        --link-port) LINK_PORT="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
        *) PASSTHROUGH+=("$1"); shift ;;
    esac
done
case "$MODE" in
    baseline|bountyhunter) ;;
    *) die "unknown mode '$MODE' - expected baseline or bountyhunter" ;;
esac

# ── the ROM, the one thing the player must supply ────────────────────────────
ROM="$(ls "$REPO"/rom/*.nds 2>/dev/null | head -1)"
if [ -z "$ROM" ]; then
    die "no ROM found.

Put a LEGO Star Wars II: The Original Trilogy (USA) cartridge dump in
    $REPO/rom/
and run this again. That is the only thing this port needs from you."
fi
got="$(sha1sum "$ROM" | cut -d' ' -f1)"
if [ "$got" != "$ROM_SHA1" ]; then
    die "that ROM is not the build this port was compiled for.

    found    $got
    expected $ROM_SHA1   (LEGO Star Wars II - The Original Trilogy (USA))

The recompiled code is generated from one exact cartridge, so a different dump
would not match it. This is checked rather than trusted because the failure
would otherwise be silent and bizarre."
fi

# ── which executable, and why the mode decides it ────────────────────────────
#
# THE MODE IS THE EXECUTABLE NAME. The runner reads its own argv[0] and sets
# LSW2_FORK=1 when it contains "bountyhunter"; frontend.cpp reads that env var
# and nothing else. So `--mode bountyhunter` running lsw2.exe gave the fork's
# SAVE to the BASELINE game - unlocks and all, but no arena lobby, no host
# settings, no fork tick. Every test run launched that way was measuring the
# wrong build, and the log said so only by omission.
#
# AND IT COMES OUT OF build/, not out/work/port-build/. That is where
# port/build.sh puts the finished pair, and where the exported fonts land -
# running the intermediate copy meant "[font] not found" and in-game menus
# drawn as empty frames, which is the fork's whole presentation. It is also the
# copy PlayLSW2.bat runs, so the launcher and the double-click now agree.
if [ "$MODE" = "bountyhunter" ]; then
    EXE="$REPO/build/lsw2_bountyhunter.exe"
else
    EXE="$REPO/build/lsw2.exe"
fi
[ -x "$EXE" ] || EXE="$REPO/out/work/port-build/lsw2.exe"

if [ -n "${USE_FRAMEWORK_FRONTEND:-}" ] || [ ! -x "$EXE" ]; then
    EXE="$FALLBACK_EXE"
    SHELL_NAME="the framework's frontend"
else
    SHELL_NAME="our shell"
fi
[ -x "$EXE" ] || die "no executable at $EXE

Build it first:  port/build.sh"

# ── one instance at a time ───────────────────────────────────────────────────
# Each launch opens its own pair of windows, so running it twice leaves two
# games stacked on the desktop with no obvious way to tell which is which.
# Refuse instead, and say how to take over.
#
# EXCEPT WHEN --instance SAYS OTHERWISE. That flag exists to run a second
# machine against the first - it already gets its own save and its own
# settings file for exactly that reason - and refusing to start it made the
# one thing it is for impossible. A two-machine test is the only way to see
# whether a client follows the host into a round, and it is how the round
# start was found to reach nobody but the host.
RUNNING="$(tasklist 2>/dev/null | grep -icE "^(lsw2|nds_runner)\.exe" || true)"
if [ "$INSTANCE" = "1" ] && [ "${RUNNING:-0}" -gt 0 ] 2>/dev/null; then
    if [ -n "${LSW2_REPLACE:-}" ]; then
        echo "closing the running instance first (LSW2_REPLACE set)"
        taskkill //F //IM lsw2.exe      >/dev/null 2>&1 || true
        taskkill //F //IM nds_runner.exe >/dev/null 2>&1 || true
        sleep 1
    else
        die "the port is already running.

Close that window first, or re-run with LSW2_REPLACE=1 to take over:
    LSW2_REPLACE=1 port/launch.sh ${MODE:+--mode $MODE}

Only one instance is allowed because each opens its own top and bottom
windows, and two sets stacked on the desktop is nobody's idea of a good time."
    fi
fi

# ── assets: only needed for modding, and only extracted on request ───────────
if [ "$EXTRACT" = "1" ]; then
    echo "extracting assets for modding..."
    python "$REPO/tools/build_asset_rom.py" || die "asset extraction failed"
    echo
    echo "Search them with:  python tools/asset_find.py <name>"
    echo "Override them by dropping <asset_id>.bin in port/assets/override/"
    exit 0
fi

# ── BIOS: dumps if you have them, FreeBIOS if you do not ────────────────────
BIOS_ARGS=(--generated-firmware)
if [ -f "$PORT/bios/biosnds9.rom" ] && [ -f "$PORT/bios/biosnds7.rom" ]; then
    BIOS_SOURCE="your retail dumps (the oracle-diffed path)"
else
    BIOS_ARGS+=(--freebios)
    BIOS_SOURCE="FreeBIOS, embedded - no dumps needed"
fi

# ── saves: shared directory, one file per mode so they cannot collide ────────
mkdir -p "$PORT/saves"
SAVE="$PORT/saves/$MODE.sav"
[ "$INSTANCE" != "1" ] && SAVE="$PORT/saves/$MODE-$INSTANCE.sav"
[ "$FRESH" = "1" ] && rm -f "$SAVE"

if [ "$MODE" = "bountyhunter" ] && [ ! -f "$SAVE" ]; then
    TEMPLATE="$PORT/saves/unlocked-template.sav"
    [ -f "$TEMPLATE" ] || die "no unlock template at $TEMPLATE"
    cp "$TEMPLATE" "$SAVE"
    echo "seeded $MODE save from the unlocked template"
fi

# THE SHELL STARTS ON THE SOFTWARE RASTERISER, and switches only when it can
# present the GPU frame itself. The framework's default renderer is the OpenGL
# compute backend, and with that active the TOP SCREEN IS PRESENTED THROUGH GL
# (nds_compute_host_present_top) while only the bottom goes through SDL
# textures - so a shell presenting from the CPU framebuffer alone shows a
# working touch screen and an empty top one, with both windows open.
#
# So the default here is soft, which is also the framework's stated accuracy
# floor. When the Video tab's "OpenGL renderer" setting is on, the shell's own
# GL presenter (frontend/glpresent.cpp) switches the policy to auto before the
# renderer starts and presents the GL frame directly; if the compute renderer
# fails to start it falls back to soft and says so in the startup log.
if [ "$SHELL_NAME" = "our shell" ]; then
    export NDS_3D_RENDERER="${NDS_3D_RENDERER:-soft}"
    RENDERER_NOTE="  (OpenGL present path available; see the Video tab)"
else
    RENDERER_NOTE=""
fi

echo "mode   : $MODE"
echo "rom    : $(basename "$ROM")  (sha1 verified)"
echo "bios   : $BIOS_SOURCE"
echo "shell  : $SHELL_NAME$RENDERER_NOTE"
echo "save   : $SAVE"
echo

# ONE SETTINGS FILE for both launchers and both modes - settings are meant to
# be shared, and a path resolved against the working directory does not
# give that. See the note in frontend.cpp.
#
# A SECOND INSTANCE IS THE ONE EXCEPTION, and it is a test affordance rather
# than a change to what ships: two copies on one machine need different names
# and different listen ports, which are settings. Instance 1 is untouched.
export LSW2_SETTINGS="$PORT/settings.json"
if [ "$INSTANCE" != "1" ]; then
    export LSW2_SETTINGS="$PORT/settings-$INSTANCE.json"
    if [ ! -f "$LSW2_SETTINGS" ]; then
        # Seeded rather than left to defaults, because the defaults are exactly
        # what collides. The port fills in everything else on first save.
        printf '{"mp_listen_port": %d, "mp_player_name": "Player%s"}\n' \
            $((28550 + INSTANCE - 1)) "$INSTANCE" > "$LSW2_SETTINGS"
        echo "seeded $LSW2_SETTINGS (port $((28550 + INSTANCE - 1)))"
    fi
fi

# TWO CONSOLES ON ONE EMULATED RADIO.
#
# WHAT THIS IS FOR, in plain English. The goal is two machines in one
# REAL arena match - the cartridge's own wireless multiplayer - rather than two
# copies warped onto the same map, which was measured to be two single-player
# arenas sitting next to each other. The cartridge talks to Nintendo's WH layer,
# WH talks to the ARM7, and the ARM7 drives a radio. The framework emulates that
# radio (melonDS's Wifi model) and can fan its MP frames out to OTHER LOCAL
# INSTANCES: wifi_net.h's `local_wireless_base_port + instance_index`. So two
# copies of this port on one machine can hear each other exactly the way two DS
# consoles in the same room do.
#
# WITHOUT THESE TWO FLAGS THE RADIO IS DEAF. The runner defaults local wireless
# off, and every run so far has said "[network] disabled (--network off)" - so
# the game's own session start could bring WH up to MEASURECHANNEL and still
# never find a peer, because there was nothing on the other end to find.
#
# THE INDEX IS ZERO-BASED and INSTANCE is one-based, hence the subtraction; the
# port numbering below already follows the same convention.
LOCAL_WIRELESS=(--local-wireless on --instance-index $((INSTANCE - 1)))

# INTERNET PLAY: --host / --join wire the in-process relay and the WH role at
# launch, mirroring the proven relay_admit topology. Host is wireless
# instance 0, joiner instance 1; the relay catches base+peer and injects
# base+self. On ONE machine two copies must use different --wireless-base values
# (and --instance 2 for the second); across two machines the same base is fine.
if [ -n "$NET_ROLE" ]; then
    export LSW2_RELAY_LINK_PORT="$LINK_PORT"
    export LSW2_RELAY_BASE="$WBASE"
    # The in-game connect UI routes through the fork's own relay path:
    # "Join by Room Code/IP" uses join_relay (real WH over the relay, NO Path A),
    # since Path A's lobby/roster/seed fights the real WH and bounces the match to
    # -1. LSW2_RELAY_AUTO turns that on; LSW2_WIRELESS_BASE tells the fork the base
    # (== --local-wireless-port). The env relay above still stands up the link at
    # boot for the known peer; AUTO just steers the in-game join off Path A.
    export LSW2_RELAY_AUTO=1
    export LSW2_WIRELESS_BASE="$WBASE"
    if [ "$NET_ROLE" = host ]; then
        export LSW2_WH_ROLE=host
        export LSW2_RELAY=host
        export LSW2_RELAY_LOCAL=0
        export LSW2_RELAY_REMOTE=1
        WIDX=0
        echo "internet HOST: wireless base $WBASE, relay listening on link port $LINK_PORT"
        echo "  the joiner runs:  port/launch.sh --mode $MODE --join <this machine's IP>"
        echo "  (make link port $LINK_PORT reachable - port-forward or UPnP it)"
    else
        [ -n "$NET_PEER" ] || die "--join needs the host's address, e.g. port/launch.sh --mode $MODE --join 203.0.113.5"
        export LSW2_WH_ROLE=join
        export LSW2_RELAY=connect
        export LSW2_RELAY_HOST="$NET_PEER"
        export LSW2_RELAY_LOCAL=1
        export LSW2_RELAY_REMOTE=0
        WIDX=1
        echo "internet JOIN: connecting to $NET_PEER:$LINK_PORT, wireless base $WBASE"
    fi
    # The relay's local/instance index is fixed by the role; override the
    # instance-derived index so --host/--join alone are enough.
    LOCAL_WIRELESS=(--local-wireless on --local-wireless-port "$WBASE" --instance-index "$WIDX")
fi

# THE PLAYER'S NAME, THE WAY THE CONSOLE CARRIES IT. The player's name is set
# on the imgui Multiplayer tab and defaults to 'Maclunkey'; the baseline has the
# same tab. The cartridge takes its nickname from the DS user
# settings the firmware loader leaves in RAM, and the runner's own
# --player-name patches the generated firmware with it before boot
# (nds_apply_player_name, ten characters, letters/digits/spaces) - so the
# beacon a peer browses, the host row it picks and the four peer-name slots all
# read the imgui name instead of the firmware's default, which had been
# seen advertising "ndsrecomp - Coop". Read from the same settings file the
# Multiplayer tab writes; the fork's dsname.cpp keeps the RAM copy in step if
# the name is changed after boot.
PLAYER_NAME="$(python -c "import json,sys; print(json.load(open(sys.argv[1], encoding='utf-8-sig')).get('mp_player_name', 'Maclunkey'))" "$PORT/settings.json" 2>/dev/null || true)"
PLAYER_NAME="$(printf '%s' "$PLAYER_NAME" | tr -cd 'A-Za-z0-9 ' | cut -c1-10 | sed 's/^ *//; s/ *$//')"
[ -n "$PLAYER_NAME" ] || PLAYER_NAME="Maclunkey"

cd "$PORT"

# DRY RUN: print the launch config and stop, without starting the game. Used to
# check the --host/--join wiring headlessly. Set LSW2_DRYRUN=1.
if [ -n "${LSW2_DRYRUN:-}" ]; then
    echo "[dryrun] exe: $EXE"
    echo "[dryrun] wireless: ${LOCAL_WIRELESS[*]}"
    echo "[dryrun] debug port: $((19842 + INSTANCE - 1))"
    echo "[dryrun] player: $PLAYER_NAME"
    echo "[dryrun] relay env: LSW2_RELAY=${LSW2_RELAY:-} LSW2_WH_ROLE=${LSW2_WH_ROLE:-} base=${LSW2_RELAY_BASE:-} local=${LSW2_RELAY_LOCAL:-} remote=${LSW2_RELAY_REMOTE:-} host=${LSW2_RELAY_HOST:-} link=${LSW2_RELAY_LINK_PORT:-}"
    exit 0
fi

exec "$EXE" "$PORT/bios" \
    --rom "$ROM" \
    --config "$PORT/game.toml" \
    --save-path "$SAVE" \
    --boot direct     --port $((19842 + INSTANCE - 1)) \
    "${BIOS_ARGS[@]}" \
    "${LOCAL_WIRELESS[@]}" \
    --player-name "$PLAYER_NAME" \
    --interactive \
    --screen-layout separate \
    ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}
