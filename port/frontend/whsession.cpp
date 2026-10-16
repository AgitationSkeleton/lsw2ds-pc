/* See whsession.h. */

#include "whsession.h"

#include "cheats.h"
#include "coophosts.h"
#include "coopjoin.h"
#include "gearhop.h"         /* WH_End on the guest's thread (session end) */
#include "guestcall.h"
#include "levels.h"
#include "netcount.h"
#include "screenengine.h"   /* lsw2_screen_engine - the stock join screens */
#include "statelog.h"
#include "statereq.h"
#include "viewmanager.h"
#include "viewmask.h"
#include "ui.h"
#include "../net/lsw2_net_bind.h"   /* lsw2_vwh_enabled */
#include "../net/lsw2_vwh.h"        /* the session a joiner waits to be seated in */

#include "hiddenbgs.h"       /* patch 0016's layer mask, shared with arenahud */

#include "state.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>

/* The cartridge's own readiness poll and its own state requester.
 *
 * CALLED BY SYMBOL, which is what makes the override below safe: a call by
 * symbol does not re-enter the dispatch table, so the poll cannot recurse into
 * its own replacement. script_enqueue_wrapper in dispatch_overrides.cpp uses
 * the same device for the same reason. */
extern "C" void lsw2_arm9_FUN_020ad438(void);
extern "C" void lsw2_arm9_FUN_0205ecc4(void);

/* WH_End (0x020ADD78) USED TO BE DECLARED HERE AND CALLED BY SYMBOL from the
 * fork tick, "lifted and therefore callable". That was RETRACTED: a lifted function run from the host takes whatever CPU context is current
 * and checks R[15] against its entry, so that call crashed the ARM7 or the
 * ARM9 at the end of a session. It is a gear hop now - see
 * lsw2_wh_end_session. */
extern "C" void lsw2_arm9_FUN_020adca4(void);
extern "C" void lsw2_arm9_FUN_020adc88(void);   /* WH vtable[0x4C]: the WM-enable */
extern "C" void lsw2_arm9_FUN_0208e374(void);
extern "C" void lsw2_arm9_FUN_020d8748(void);   /* WM cmd 0x19 (SetBeaconIndication?): fires any state>=2, ineffective */
extern "C" void lsw2_arm9_FUN_020d8870(void);   /* WM_SetEntry(callback, enable): FIFO cmd 0x21, needs parent state {2,7,9} */
extern "C" void lsw2_arm9_FUN_020ae4d4(void);   /* host MP-start / WH state handler that calls WM_StartMP */

/* The cartridge's view-manager allocator - see lsw2_wh_make_view_manager in
 * the header for why this one and not the session start that calls it. */
extern "C" void lsw2_arm9_FUN_0207281c(void);

namespace {

/* How many times the real poll may say "not ready" before its answer is
 * overruled. Not zero, because if a future build ever does bring a radio up
 * the game's own answer should win; not large, because the session start is
 * spinning on this and every extra round trip is a frame the guest is inside
 * a loop. */
/* The probe: asked once, a second after the lobby settles, then the branch
 * ring is dumped a few frames later - by which time the death has happened and
 * the ring holds it.
 *
 * DUMPED FROM INSIDE THE PORT, not over the debug socket. The socket was the
 * obvious way and it does not work: once the guest is wedged, `debug_pump()`
 * runs between frames and those have stopped, so `runtime_trace` times out.
 * The emulator loop is still turning - the log keeps printing unmapped reads -
 * so the fork tick is still called, and `runtime_trace_copy_recent` is public
 * in runtime_arm.h. */
bool g_probe_asked = false;
int g_probe_wait = 0;
int g_probe_dump_in = -1;
constexpr int kProbeAfterFrames = 120;
/* TWO FRAMES, NOT EIGHT. The ring holds 4096 entries and the ARM7 fills it
 * faster than that; by frame eight the ARM9's death was long gone. */
/* WHERE THE RING IS ARMED, AND HOW LONG AFTER THAT IT IS READ.
 *
 * Measured: the fork tick runs 35 more frames after the request and then no
 * frame is ever produced again, with 64 unmapped reads at the end. Arming at
 * 30 and reading at 38 puts the whole of that window inside a ring the ARM7
 * has not had time to fill. */
/* ARM AT 32, READ AT 34. The first attempt armed at 30 and read at 38, and
 * the read never came: the tick stops at 35, so anything scheduled after that
 * simply does not happen - which is itself a second, independent confirmation
 * of the 35. The window has to close before the stall, not after it. */
/* HOW OFTEN THE STATE REQUEST REPEATS, AND HOW MANY TIMES.
 *
 * The machine consumes a request when it chooses - 34 frames of PENDING were
 * measured - and sometimes never. Asking once was the whole reason the
 * two-console runs looked flaky. Half a second between attempts is
 * comfortably longer than the observed consume latency, so a repeat never
 * stacks a second request on a live one; twenty attempts is ten seconds, long
 * enough that failing all of them is a real fact rather than impatience. */
constexpr int kProbeAskGap = 30;
constexpr int kProbeMaxAsks = 20;

constexpr int kProbeArmAt = 32;
constexpr int kProbeReadAfter = 2;
constexpr uint32_t kProbeTraceEntries = 1024;
constexpr uint32_t kProbeShowLast = 40;

/* The node trace collapses repeats, so it can afford a much wider window than
 * the state-54 probe - a level load is thousands of branches and the useful
 * call sites are scattered through them. */
constexpr uint32_t kNodeShowLast = 400;

/* The whole ring, and MAIN RAM ONLY.
 *
 * The first two dumps showed nothing but ITCM: 0x01FF8xxx is the allocator, and
 * a level load calls it thousands of times, so it fills a 4096-entry ring on its
 * own and buries every game-code call site. Excluding ITCM at the dump makes the
 * window worth having - the question is which GAME function builds the table,
 * and malloc is not it. */
constexpr uint32_t kNodeTraceEntries = 4096;
constexpr uint32_t kNodeCodeLow = 0x02000000u;

/* How long the node-table trace may leave deep tracing on. Small on purpose:
 * recording every branch on both cores stops the run if it is left running, and
 * the window between the level coming back and the table appearing is only a
 * handful of frames. */
constexpr int kNodeTraceFrames = 24;

/* State 54's object fields are written as literals below rather than named
 * constants, because dup_constants flags +0xEC against dispatch_overrides.cpp's
 * pause cursor - two unrelated structures that happen to share an offset, and a
 * shared name would make that coincidence look like a relationship. From the
 * constructor: [0x3b] and [0x3c] as words, so +0xEC is the SELECTION that
 * FUN_0208EA58 switches on (1 = its wireless case) and +0xF0 the DEFAULT
 * target, which the constructor sets to 0x39. */
bool g_probe_selected = false;

constexpr int kPollGrace = 2;
int g_polls = 0;

/* THE TRAMPOLINE'S FOUR STATES. Idle -> Wanted (the fork has asked) ->
 * Running (the guest's PC is inside the allocator) -> Ran (it came back) ->
 * Done (reported, one way or the other). It is a small state machine rather
 * than a bool because the interesting outcome - "it finished and produced
 * nothing" - is only distinguishable from "it has not finished yet" if the
 * return is recorded separately from the request. */
enum TrampState { kTrampIdle, kTrampWanted, kTrampRunning, kTrampRan,
                  kTrampDone };
TrampState g_tramp = kTrampIdle;

/* THE HOST PRESSED START GAME AND THE SESSION WAS NOT UP YET.
 *
 * take_start_request() yields the press exactly once, so the console route has
 * to remember it or the retry never comes - measured: the route ran, the
 * function said it was bringing the session up, and nothing ever asked again.
 * Cleared when the chooser is actually open. */
bool g_console_start_pending = false;
/* The joiner's counterpart, for the same reason. */
bool g_console_join_pending = false;
/* Whether the browse-list request has actually gone out. Arrival is only real
 * after this: the session bringup passes through the browse state on its own. */
bool g_console_join_asked = false;

/* HOW THE CONSOLE ROUTE PACES ITS REQUESTS. Half a second between asks and a
 * bounded number of them - the same shape lsw2_wh_probe_chooser already proved,
 * adopted here after a write watch caught both console routes posting a fresh
 * request every single frame. */
constexpr int kConsoleAskGap = 30;
constexpr int kConsoleMaxAsks = 20;
int g_console_join_gap = 0;
int g_console_join_asks = 0;
int g_console_start_gap = 0;
int g_console_start_asks = 0;

/* The host function's world, saved across the hop. The allocator is an
 * ordinary AAPCS function and will clobber r0-r3 and lr; the host is entered
 * again afterwards expecting its own arguments, so they have to come back. */
uint32_t g_saved_lr = 0;
uint32_t g_saved_r[4] = {0, 0, 0, 0};


/* WHICH MENU THE HOST ASKS FOR. Default 0x36 - the screen this project has
 * been driving, and the one that currently gets two consoles connected.
 *
 * THE CARTRIDGE'S OWN WIRELESS MENU IS STATE 53 (0x35), NOT 54. The state
 * factory FUN_0205F574 is `cmp r9,#0x39; addls pc,pc,r9,lsl #2`, so case N's
 * branch sits at 0x0205F59C + N*4. Case 53 is `b 0x0205FE00`, whose arm
 * allocates 0xF4 bytes and calls `bl 0x0208E260` at 0x0205FE28 - the
 * constructor that installs vtable 0x020FF524, whose handler is FUN_0208EA58,
 * whose case 1 calls the session start 0x020B10A8 and then asks for state 15.
 * Case 54 branches to 0x0205FC94 instead, a different class with vtable
 * 0x020FFB10 - which is what was measured on the live object.
 *
 * So the screen the fork drives is NOT the one the cartridge hosts from, and
 * that is why the fork has had to call the session start itself. This makes the
 * real route selectable without disturbing the one that already works. */
uint32_t host_menu_state() {
    static const uint32_t menu = [] {
        const char* e = std::getenv("LSW2_WH_MENU");
        const long n = e ? std::strtol(e, nullptr, 0) : 0;
        return (n > 0 && n < 0x3A) ? uint32_t(n) : 0x36u;
    }();
    return menu;
}

/* THE CARTRIDGE'S OWN MENU DOES ITS OWN SESSION START, so the fork must not
 * also do it. When the host is sent to state 53, FUN_0208EA58 calls
 * 0x020B10A8 itself; the hop then only has to provide the view manager, which
 * is exactly what the join role already does. */
bool cartridge_menu_starts_session() { return host_menu_state() != 0x36u; }

uint32_t read32(uint32_t a) { return bus_device_read32(0, a); }

void write32(uint32_t a, uint32_t v) { bus_device_write32(0, a, v); }


const char* const kStateNames[] = {
    "STOP", "IDLE", "SCANNING", "BUSY", "CONNECTED",
    "DATASHARING", "KEYSHARING", "MEASURECHANNEL", "DISCONNECTED", "ERROR",
};
constexpr int kStateCount = int(sizeof kStateNames / sizeof *kStateNames);

}  // namespace

bool lsw2_wh_enabled() {
    /* ON BY DEFAULT. THE GATE'S OWN CONDITION WAS MET AND IT WAS NEVER
     * LIFTED, and this is the correction.
     *
     * It read `e && *e == '1'` - off unless asked for - and whsession.h named
     * the condition for changing that: "the gate comes off when a drive shows
     * the state machine advancing and the guest healthy". That drive happened.
     * The state machine advances, the flow reaches the match, and a full
     * deathmatch has run end to end with both consoles agreeing on world
     * positions and the cross-player error word reading zero throughout.
     *
     * WHY THIS IS THE HIGHEST-VALUE LINE IN THE PORT. lsw2_fork_begin_round
     * tries lsw2_wh_console_start() first and WARPS if it returns false, and
     * this function is that route's first condition. So with the gate shut,
     * every ordinary launch took the warp - and the requirement is that the
     * players are actually connected and present in the real arena multiplayer
     * system, not just warped to the map. The real path existed, was written
     * for exactly that, and was measured working; it was simply unreachable
     * without an environment variable that nothing shipped sets.
     *
     * THE OPT-OUT IS KEPT, AND INVERTED RATHER THAN REMOVED. `LSW2_REAL_WH=0`
     * restores the old behaviour exactly, because a bad bring-up has been seen
     * to wreck the guest and an instrument that cannot be switched off
     * is one that cannot be bisected. Any other value, and absence, mean on.
     *
     * THE BASELINE IS UNAFFECTED BY CONSTRUCTION, which is why this flip is
     * safe to make in one line: every entry point in this file also tests
     * ui_fork_build(), and the dispatch registration at dispatch_overrides.cpp
     * is `if (fork && lsw2_wh_enabled())`. The base recomp plays exactly as it
     * normally does without anything further being done. */
    static const bool on = [] {
        const char* e = std::getenv("LSW2_REAL_WH");
        return !(e && *e == '0');
    }();
    return on;
}

int lsw2_wh_state() {
    return int(read32(kLsw2SessionAuthority + kLsw2WhSystemState));
}

const char* lsw2_wh_state_name(int state) {
    /* THE NAMES ARE THE CARTRIDGE'S, transcribed from its own table at
     * 0x020FFD20 and read out of a running instance rather than assumed from
     * the NitroSDK headers - including the typo in entry 8, which is the
     * cartridge's and is left alone because matching it is how a reader knows
     * this list came from the ROM. */
    if (state < 0 || state >= kStateCount) return "?";
    return kStateNames[state];
}

void lsw2_wh_report() {
    if (!ui_fork_build()) return;

    /* THE ERROR CODE TOO, and it is not decoration.
     *
     * The match start fails with the cartridge's "Communication Error" while
     * the netgame streams on both consoles are identical and bidirectional
     * right up to the moment WH leaves DATASHARING for BUSY and never returns.
     * Whether the WH layer recorded a reason for that is one read
     * away, and it is the difference between "the radio refused" and "the game
     * decided" - which are different bugs with different fixes.
     *
     * Reported on any change to EITHER field, because an error can be latched
     * without the state moving, and a state can move without an error. */
    /* AND THE STEP SIGNAL, for the same reason the error field was added here.
     *
     * The goal this serves, in plain english: when the match-start transition
     * fails, the state alone says only "BUSY" - the host sat in BUSY for the
     * last two thousand lines of a run without that ever naming what it was
     * waiting on. The step signal says which WH operation last COMPLETED, so
     * a stall becomes a sentence instead of a symptom.
     *
     * It earns its place by having already disagreed across the two consoles:
     * after the same failure the host is parked at step 0x09 with no error and
     * the child at 0x1C with error 3 - two different stalls that the state
     * field reported identically as BUSY on both. Reported on a change to ANY
     * of the three, because a step can advance without the state moving. */
    /* A STEP-ONLY CHANGE IS SUMMARISED, NOT STAMPED. Once data-sharing runs,
     * the step signal alternates between two operations every few frames
     * (0x29 <-> 0x2D on the host, measured) with the state and error
     * unchanged - ten lines a second that each say nothing new, and cost the
     * host frame time. A state or error change is still reported the moment
     * it happens; a step that only flips is reported at most once a second,
     * with how many flips that line stands for, so a stall still shows as the
     * step that STOPPED moving. */
    static int last_state = -1;
    static uint32_t last_err = 0xFFFFFFFFu;
    static uint32_t last_step = 0xFFFFFFFFu;
    static int step_flips = 0;
    static int frames_since_line = 0;
    constexpr int kStepLineFrames = 60;
    ++frames_since_line;
    const int s = lsw2_wh_state();
    const uint32_t err = read32(kLsw2SessionAuthority + kLsw2WhErrorCode);
    const uint32_t step = read32(kLsw2SessionAuthority + kLsw2WhStepSignal);
    if (s == last_state && err == last_err && step == last_step) return;
    const bool step_only = s == last_state && err == last_err;
    last_state = s;
    last_err = err;
    last_step = step;
    if (step_only && frames_since_line < kStepLineFrames) {
        ++step_flips;
        return;
    }
    frames_since_line = 0;
    if (step_flips > 0) {
        std::fprintf(stderr,
                     "[fork] WH state -> %d WH_SYSSTATE_%s  error=%08X  step=%02X"
                     "  (+%d step changes)  t=%s\n",
                     s, lsw2_wh_state_name(s), unsigned(err), unsigned(step),
                     step_flips, lsw2_log_clock());
        step_flips = 0;
        return;
    }
    std::fprintf(stderr,
                 "[fork] WH state -> %d WH_SYSSTATE_%s  error=%08X  step=%02X"
                 "  t=%s\n",
                 s, lsw2_wh_state_name(s), unsigned(err), unsigned(step),
                 lsw2_log_clock());
}

void lsw2_wh_ready_poll() {
    /* THE GAME'S OWN POLL FIRST, ALWAYS. It allocates two buffers - 0xC80 and
     * 0x220 - into the game's heap and clears eight fields of the WH context,
     * and every one of those is something the rest of the layer will read.
     * Only its return value is in question here.
     *
     * AND IT MAY WELL ANSWER FOR ITSELF NOW. This override was written for a
     * host call, where the poll could never succeed because it waits on the
     * ARM7 and a host call advances only the ARM9. On the guest's own thread,
     * across frames, both cores run and the WM handshake has a real chance of
     * completing - the framework emulates the ARM7 and the IPC FIFO, and the
     * boot log even says "Wi-Fi device is live". So the grace below is not a
     * formality: if the cartridge can answer honestly, it wins. */
    g_cpu.R[15] = kLsw2WhReadyPoll;
    lsw2_arm9_FUN_020ad438();

    if (g_cpu.R[0] == 0) {
        if (g_polls < kPollGrace) {
            ++g_polls;
        } else {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr,
                             "[fork] WH readiness: the cartridge's poll said "
                             "not ready %d times - answering ready. See "
                             "whsession.h\n", kPollGrace);
            }
            g_cpu.R[0] = 1;
        }
    }

    /* Return the way a lifted function does, or the caller resumes somewhere
     * else entirely. */
    g_cpu.R[15] = g_cpu.R[14];
}

namespace {
/* THE UI'S CHOICE OF ROLE, when it has made one. 0 = not chosen (fall back to
 * the env default); 1 = host; 2 = join. Set by lsw2_wh_set_role from the fork's
 * Host / Join menus. */
int g_ui_wh_role = 0;
}  // namespace

void lsw2_wh_set_role(bool is_host) {
    const int want = is_host ? 1 : 2;
    if (g_ui_wh_role == want) return;
    g_ui_wh_role = want;
    std::fprintf(stderr,
                 "[fork] wireless role set from the UI: %s - so two windows take "
                 "opposite roles and the joiner scans for the host\n",
                 is_host ? "HOST" : "JOIN");
}

bool lsw2_wh_role_is_host() {
    /* THE UI DECIDES ONCE IT HAS ACTED - this is the fix for UI-driven,
     * one-machine, two-window play. Two instances started from the
     * in-game Host/Join menus must take OPPOSITE roles, but this used to be a
     * cached read of LSW2_WH_ROLE that defaults HOST - so BOTH windows became
     * access points, the joiner never scanned for the host's beacon, and the two
     * cartridge sessions never connected (players invisible to each other).
     * start_lobby now sets HOST and join_address sets JOIN.
     *
     * FALLBACK UNCHANGED: absent a UI action, the env default stands, so
     * launch.sh --host/--join and a lone console behave exactly as before - a
     * lone console with nothing to join still brings its own session up. */
    if (g_ui_wh_role != 0) return g_ui_wh_role == 1;
    const char* e = std::getenv("LSW2_WH_ROLE");
    return !(e && (*e == 'j' || *e == 'J'));
}

/* CLEAR THE CROSS-PLAYER CHECK'S GATE IF IT IS SET, and say so once.
 *
 * Shared by the two call sites below so that both consoles restore the same
 * invariant by the same code. Returns true when it actually cleared one.
 *
 * WRITTEN, NOT CALLED. `FUN_020af864` - the vtable +0x18 setter the cartridge
 * uses - is a one-line `*(u8 *)(obj + 0x6F4) = arg`, so a direct byte store is
 * exactly what a virtual call would do, without a guest call that could
 * mistime. Three earlier changes that reached into the guest with a call or
 * a state request had to be reverted; a byte store needs no hop. */
static bool net_check_disarm(const char* why) {
    const uint32_t netgame = read32(kLsw2ViewManagerPtr);
    if (netgame == 0) return false;

    const uint32_t addr = netgame + kLsw2NetCheckGate;
    const uint32_t word = read32(addr & ~3u);
    const unsigned shift = (addr & 3u) * 8u;
    if (((word >> shift) & 0xFFu) == 0u) return false;

    write32(addr & ~3u, word & ~(0xFFu << shift));

    /* Rate-limited rather than silenced: the gate is only ever re-armed by a
     * level load, so this should print once per console per session, and a
     * second line would itself be the finding. */
    static int said = 0;
    if (said < 4) {
        ++said;
        std::fprintf(stderr,
                     "[fork] %s: disarmed the cross-player check at %08X, "
                     "which the fork's lobby route left armed (see "
                     "viewmask.h)\n",
                     why, unsigned(addr));
    }
    return true;
}

void lsw2_net_check_disarm_prematch() {
    /* THE SAME REPAIR, ON WHATEVER CONSOLE IS SITTING IN THE FORK'S LOBBY.
     *
     * The goal this serves, in plain english: a match cannot start while
     * either console's cross-player check is armed, and the fork arms both of
     * them by accident. Its lobby IS a loaded level (the cantina, level 9), and
     * the level load runs FUN_0208c888, which sets the gate WITHOUT re-seeding
     * the shared random generator first. Every other site in the cartridge that
     * arms it seeds in the same breath (FUN_02098b6c), and every site that
     * draws disarms as it draws (FUN_020b0cd4). So the fork leaves the check
     * live across a window where the two generators are legitimately apart, and
     * the check duly fails.
     *
     * MEASURED ON BOTH CONSOLES, not assumed. A write watch on each console's
     * OWN gate - the host's netgame object is 0x021E08C8 so its gate is
     * 0x021E0FBC, the child's is 0x0224391C so its gate is 0x02244010 - caught
     * the identical writer on each:
     *
     *     [watch] 02244010 = 1 pc=020AF870 lr=0208CA90 r0=0224391C r5=0224391C
     *
     * `lr=0208CA90` is the return of the BLX inside FUN_0208c888. And the word
     * the failure actually turns on, 0x021092FC, read 0 on the host (which the
     * Game Settings disarm below had already cleared) and 2 on the child, where
     * 2 is "the cross-player consistency check FAILED" - a value FUN_020af330
     * can only return while this gate is set. So this is the child's fault too,
     * and it is not a dead link.
     *
     * ONLY THE TWO PRE-MATCH SCREENS. The arena lobby (48) and Game Settings
     * (54) are both before any seed has been shared, so the check has nothing
     * meaningful to compare and must not be armed. State 41 - the real match -
     * is deliberately NOT in this list: its own class re-seeds and re-arms in
     * the correct order, and that arming is the one that should stand. */
    if (!lsw2_wh_enabled() || !ui_fork_build()) return;

    /* Extending this across the 3-station reload states (34/39/40) was tried and
     * REVERTED: the 3-station state-38 is NOT this checksum check - the
     * error word 0x021092FC read 1, not 2 (FAILED), and the WH carried error=5, so
     * the collapse is a WH-level data-share failure, not the pre-seed consistency
     * check this disarms. Restored to the two verified pre-match screens. */
    const int st = lsw2_state_current();
    if (st != kLsw2ArenaLobbyState && st != int(host_menu_state())) return;

    net_check_disarm("the fork's cantina lobby is up");
}

bool lsw2_wh_console_start() {
    /* THE LOBBY CONSOLE'S WAY IN, instead of the fork's boot scaffolding.
     *
     * WHY THIS EXISTS, in plain English. One console used to be already
     * sitting in the map chooser while the other browsed coop lobbies, and
     * the reason was that the fork put them there: LSW2_WH_ROLE sent
     * one straight to state 54 and the other straight to state 34 at boot, and
     * neither player ever walked to the cantina's wireless console. That was
     * scaffolding for bringing the radio up at all, and it is not the intended
     * design - the players should be present in the lobby, navigating
     * it, and reaching these screens through the game's own flow.
     *
     * The flow already exists on both sides. The cartridge has the console
     * entity in the arena lobby, the player walks into it, its script fires
     * lsw2_fork_console_interact(), and the fork opens the host's console
     * menu - "Start Game", "Change Settings", "Show Room Code", "Kick
     * Player", "Shut Down Lobby", "Return". What Start Game did was
     * lsw2_fork_begin_round's warp, and a warp is exactly what this must
     * not be.
     *
     * So Start Game now brings up the CARTRIDGE'S session and opens the
     * CARTRIDGE'S Game Settings, from the console entity, when the host asks -
     * rather than at boot, before anybody has chosen anything.
     *
     * RETURNS TRUE WHEN IT HAS TAKEN OWNERSHIP of the round start, so the
     * caller does not also warp. False means the real-WH route is off and the
     * fork's own path should run unchanged - the baseline and every existing
     * test still take that path. */
    if (!lsw2_wh_enabled() || !ui_fork_build()) return false;

    /* THE SESSION FIRST, IF IT IS NOT UP. The hop runs the cartridge's own
     * 0x020B10A8 on the guest's thread for a host; until it has, there is
     * nothing to open a chooser for. Returning true while it is still coming up
     * swallows this round start deliberately: the host pressed Start Game and
     * the answer is "starting", not "warp instead". */
    /* LATCHED, BECAUSE THE TRIGGER IS CONSUMED ONCE.
     *
     * take_start_request() hands the Start Game press over exactly once, so
     * returning "taken, still coming up" and waiting for a next call meant
     * waiting forever - measured: the route ran, this function said it was
     * bringing the session up, and nothing ever asked again. The press is
     * remembered here instead and serviced every tick until the chooser is
     * actually open. */
    g_console_start_pending = true;

    const int vm = lsw2_wh_make_view_manager();
    if (vm < 0) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr,
                         "[fork] console Start Game: bringing the cartridge's "
                         "wireless session up first\n");
        }
        return true;
    }

    /* NO GUEST-READY GATE ON THIS PATH ANY MORE.
     *
     * It used to wait here for a slice boundary the guest-call seam would
     * accept, and that wait was measured never ending once a peer had
     * joined - "refused by not-ARM9 5139, halted 5095". The request below now
     * goes through the trampoline, which does not need a quiet boundary at
     * all. The gate is REMOVED rather than parked behind a flag: keeping a
     * wait that is known never to end would only reintroduce the stall.
     *
     * The instrument that found this earns its keep and is not being deleted -
     * it is the reason the stall was measured instead of guessed. Its lesson
     * lives on as a standing rule: make every silent path speak. */

    /* ARRIVED. Say so once and stop asking - "requested" and "open" are not the
     * same thing, and this file has already been caught treating them as one. */
    if (lsw2_state_current() == int(host_menu_state())) {
        /* AND DISARM THE CONSISTENCY CHECK, because the route that got us here
         * did not.
         *
         * The goal this serves, in plain english: pressing Start Game on this
         * screen draws from the shared random generator and broadcasts the new
         * seed. Every cartridge site that draws disarms the cross-player check
         * as it draws, because a draw moves this console off the seed the peer
         * still holds - see kLsw2NetCheckGate for the ROM's own pattern. The
         * retail path onto this screen arrives disarmed; the fork's does not,
         * because its lobby is a loaded level (which arms the check) and the
         * cantina console entity opens Game Settings without going through the
         * message whose sender would have disarmed.
         *
         * So the check sits armed across a window where the two generators are
         * legitimately apart, fires, and tears the session down before the seed
         * can land. Clearing the byte here restores the invariant the ROM keeps
         * everywhere else.
         *
         * KEPT AS ITS OWN CALL even though lsw2_net_check_disarm_prematch()
         * now covers state 54 as well. This is the site the A/B test
         * actually measured, and it fires the instant the screen is confirmed
         * open rather than on the next frame tick. The helper is idempotent, so
         * whichever runs first simply finds the byte already clear. */
        net_check_disarm("Game Settings is open");

        if (g_console_start_pending) {
            g_console_start_pending = false;
            std::fprintf(stderr,
                         "[fork] console Start Game: the cartridge's own Game "
                         "Settings (state %u) is OPEN - reached because the "
                         "host chose it at the console entity\n",
                         unsigned(host_menu_state()));
        }
        return true;
    }

    /* PACED FOR THE SAME REASON AS THE JOINER'S, and it had the same bug: the
     * tick calls this every frame while the press is latched, the printf was
     * guarded and the request was not. This one happened to arrive quickly
     * enough that the flooding never showed, which is exactly the kind of luck
     * that should not be relied on. */
    if (g_console_start_gap > 0) { --g_console_start_gap; return true; }
    if (g_console_start_asks >= kConsoleMaxAsks) {
        if (g_console_start_pending) {
            g_console_start_pending = false;
            std::fprintf(stderr,
                         "[fork] console Start Game: gave up after %d requests "
                         "for state %u\n",
                         kConsoleMaxAsks, unsigned(host_menu_state()));
        }
        return true;
    }

    static bool asked = false;
    if (!asked) {
        asked = true;
        std::fprintf(stderr,
                     "[fork] console Start Game: opening the cartridge's own "
                     "Game Settings (state %u) - the host chose this from the "
                     "console entity, not the fork's boot probe\n",
                     unsigned(host_menu_state()));
    }

    /* LSW2_DEEP_TRACE_54=1 - arm the runtime's branch ring as Game Settings is
     * requested, so the first bad access after it dumps the path that led there
     * (patch 0007). Off again after two seconds (lsw2_vwh_tick): deep trace takes
     * the slow bus path for everything. Diagnostic only. */
    if (const char* dt = std::getenv("LSW2_DEEP_TRACE_54"); dt && *dt == '1') {
        runtime_set_deep_trace(1);
        std::fprintf(stderr, "[fork] deep trace ON for the Game Settings open\n");
    }

    ++g_console_start_asks;
    g_console_start_gap = kConsoleAskGap;
    /* THROUGH THE TRAMPOLINE, NOT THE GUEST-CALL SEAM.
     *
     * This is the whole point: by the time the host picks Start
     * Game it has a peer connected, and from then on the seam refuses at
     * essentially every slice boundary because the ARM9 is halted or off-core.
     * The request simply never went out. The trampoline runs the cartridge's
     * own requester on the guest's thread with both cores live, which is how
     * the wireless session is brought up in the first place. */
    lsw2_wh_write_state_request(host_menu_state());
    return true;
}

void lsw2_wh_end_session() {
    /* END THE CARTRIDGE'S SESSION, WITH THE CARTRIDGE'S OWN FUNCTION.
     *
     * Shut Down Lobby did not gracefully return players to single player.
     * The measured reason: shut_down_lobby() is thorough
     * about the FORK'S TCP session and silent about the CARTRIDGE'S, and the
     * cartridge's is the one peers are in.
     *
     * ON THE GUEST'S THREAD - RETRACTED "A PLAIN CALL", after it crashed a
     * capture-the-flag match. This called the lifted WH_End
     * straight from the fork tick, on the grounds that a lifted function is "a
     * C function the port can call". It is not, from there: a lifted function
     * runs on whatever CPU context the scheduler left current, and it opens by
     * checking R[15] against its own entry. Between frames R[15] is wherever
     * that CPU stopped. With the ARM7 halted inside the BIOS Halt SWI it read
     * 0x000011C0, the entry switch fell to runtime_dispatch_miss, and the
     * ARM7 was halted for good - "[dispatch-miss] cpu=arm7 pc=0x000011C0
     * lr=0x00002E10", in that crash and in three earlier drives. With the ARM9
     * current it read the ARM9's own PC (0x01FFD4C0, conn_halt4). Resolved by
     * the caller address the miss was reported from: this function.
     *
     * So WH_End goes through the gear hop like every other guest call the fork
     * makes: queued, and run on the guest's own thread from the next hosting
     * override, with the ARM9's context current and R[15] set to the entry.
     * Fire and forget - the cartridge's teardown (lsw2_wh_request_session_end)
     * is the main path, and this is its belt and braces.
     *
     * NOTHING TO END IF NOTHING IS UP. The view-manager POINTER is the test,
     * not what any function returned - that distinction cost a run already. */
    if (!lsw2_wh_enabled() || !ui_fork_build()) return;
    if (!lsw2_wh_have_view_manager()) return;

    const bool queued = lsw2_gearhop_request_toggle(kLsw2WhEnd, 0u, 0u, 0u);
    std::fprintf(stderr,
                 "[fork] session end: the cartridge's own WH_End (0x020ADD78) "
                 "%s for the guest's thread - WH state is %d\n",
                 queued ? "queued" : "NOT queued (already waiting)",
                 lsw2_wh_state());
}

void lsw2_wh_console_open_lobby() {
    /* PUT THIS CONSOLE ON THE AIR, and do nothing else.
     *
     * The host's route used to bring the cartridge's session up at the same
     * moment it opened the Game Settings chooser, which put the two events in
     * the wrong order: a console advertising a game is what the other one needs
     * to find, and a console sitting in a chooser is not advertising anything.
     * The joiner's browse list duly opened, listed nothing and closed itself,
     * and that looked like a fault in the joiner rather than an absence on the
     * air.
     *
     * So opening the lobby - the menu's own "Start Lobby" - is what starts
     * the session now, and the chooser waits for Start Game. That is also the
     * order the design calls for: players gather in the
     * lobby, and the host starts the match afterwards.
     *
     * No return value because there is nothing for the caller to decide: the
     * fork's own lobby opens either way, and this only adds the radio to it. */
    if (!lsw2_wh_enabled() || !ui_fork_build()) return;

    /* THIS WINDOW IS THE HOST/ACCESS-POINT. Set from the UI so a
     * second window that Joins takes the opposite role rather than both
     * defaulting to host and never connecting. */
    lsw2_wh_set_role(true);

    const int vm = lsw2_wh_make_view_manager();
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr,
                     "[fork] console Start Lobby: putting this console on the "
                     "air (view manager %d) - a joiner's browse list can only "
                     "list a host that is actually advertising\n",
                     vm);
    }
}

/* Set by Lsw2ForkSession::join_relay: force the real DS-WM browse route
 * in console_join without LSW2_JOIN_WM, because an internet joiner reaches the host
 * only over the relay and must scan/associate the cartridge's own way. */
static bool g_force_browse = false;
void lsw2_wh_set_force_browse(bool on) { g_force_browse = on; }

bool lsw2_wh_console_join() {
    /* THE SAME QUESTION, ANSWERED FOR THE OTHER CONSOLE.
     *
     * The host half of this stopped the fork from parking a player in the map
     * chooser at boot. This is the half that stops it parking the other player
     * in the wireless browse list at boot. Both symptoms showed up together in
     * one pair of screenshots; fixing one and leaving the other would have
     * fixed half the problem.
     *
     * The way in is the arena menu's "Join by IP/Room Code" - one of the three
     * rows the multiplayer arena menu replaces the stock entries with, so no
     * row is invented for this. The player walks
     * into the cantina's console entity, picks it, and picks Connect. That
     * opens the CARTRIDGE'S own "Browsing Wireless Game" list, where the host
     * appears as a real beacon and choosing it raises the cartridge's own
     * "is trying to connect ... Yes/No" prompt. That is what connecting the
     * way the base game's bounty hunter setup does means here.
     *
     * WHAT THIS DOES NOT DO: change which console owns the radio. LSW2_WH_ROLE
     * still decides that, and it should - somebody has to be the access point.
     * What it no longer decides is which SCREEN a player is looking at, which
     * is the part that was never the fork's to choose.
     *
     * Returns true when it has taken the join, so the caller skips the fork's
     * own master-server path; false when the real-WH route is off, leaving the
     * fork's lobby join exactly as it was. */
    if (!lsw2_wh_enabled() || !ui_fork_build()) return false;

    /* THIS WINDOW IS THE JOINER. Set from the UI so it scans for the
     * host's beacon instead of becoming a second access point - the fix for two
     * UI-launched windows both defaulting to host and never seeing each other. */
    lsw2_wh_set_role(false);

    /* Latched for the same reason the host's is: the session may still be
     * coming up when the player presses Connect, and the press does not
     * repeat itself. */
    g_console_join_pending = true;

    /* NOT UNTIL THE HOST HAS SEATED US. Over the host-level WH the
     * socket connects at once, but the host seats one joiner at a time and only
     * while its game sits in the lobby - a player who connects mid-round waits
     * for the next lobby. Until START arrives this console keeps its own lobby;
     * the latch holds the request and lsw2_wh_console_start_tick brings it back
     * every frame, so the browse-and-connect runs the moment we are seated. */
    if (lsw2_vwh_enabled()) {
        const Lsw2VwhSession* seat = lsw2_vwh_session();
        if (seat && !seat->hosting() && !seat->live()) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[vwh] connected - waiting for the host to "
                             "seat this console (it seats one joiner at a time, "
                             "in its lobby)\n");
            }
            return true;
        }
    }

    const int vm = lsw2_wh_make_view_manager();
    if (vm < 0) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr,
                         "[fork] console Join: bringing the cartridge's "
                         "wireless session up first\n");
        }
        return true;
    }

    /* The view manager is up (vm >= 0). Under the PC-transport design the
     * joiner does NOT drive the cartridge's DS-WM browse list: the peer is
     * seated over TCP and surfaced via the viewmask, and the browse list only
     * parks the joiner in state 34 where the input-count derivation never
     * runs. Building the view manager here is what we actually need (it lets
     * the mask copy into the view record). Opening the real browse list is a
     * debug-only detour, gated behind LSW2_JOIN_WM=1. */
    static const bool force_wm = [] {
        const char* e = std::getenv("LSW2_JOIN_WM");
        return e && *e == '1';
    }();
    /* The host-level WH joins through the cartridge's own browse list
     * and connect - that is how the game picks its host - so it always opens. */
    if (!force_wm && !g_force_browse && !lsw2_vwh_enabled()) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr,
                         "[fork] console Join: view manager up; seating peer "
                         "over PC transport (browse list skipped, set "
                         "LSW2_JOIN_WM=1 to force the DS-WM detour)\n");
        }
        return true;
    }

    if (!lsw2_guest_ready()) return true;   /* try again next tick */

    /* THE RADIO HAS TO BE UP, NOT MERELY ALLOCATED.
     *
     * Measured, and it cost a run: the first version gated on
     * lsw2_wh_make_view_manager() alone, which answers "already" as soon as a
     * view manager exists. That is not the same fact. The request went out
     * while the radio was still off - the log has `[state] entered 34` several
     * lines BEFORE `WIFI: ON` - so the cartridge opened its browse list, found
     * no radio to scan with, and dropped straight back to the lobby: the trail
     * read 15 -> 34 -> 48 and it was first blamed on a stray test-driver
     * console visit, which was wrong.
     *
     * The host's half never hit this because its destination is a local
     * chooser; a browse list is the one screen that genuinely needs the
     * hardware before it can say anything.
     *
     * WH_SYSSTATE_STOP(0) and IDLE(1) mean there is nothing listening yet.
     * Anything from SCANNING(2) up means the cartridge's own wireless layer is
     * running, which is the precondition the list actually has. */
    const int wh = lsw2_wh_state();
    if (wh < 2) {
        static bool waited = false;
        if (!waited) {
            waited = true;
            std::fprintf(stderr,
                         "[fork] console Join: waiting for the radio before "
                         "opening the browse list (WH state %d "
                         "WH_SYSSTATE_%s) - a list opened without one finds "
                         "nothing and closes itself\n",
                         wh, lsw2_wh_state_name(wh));
        }
        return true;
    }

    /* ARRIVAL MEANS "BECAUSE WE ASKED", NOT "THE NUMBER IS 34".
     *
     * Bringing the session up transits the browse state by itself - the log
     * shows `entered 34` while the view manager is still null - so an earlier
     * version saw that number, declared the list open and stopped. That is the
     * exact mistake this file has a standing rule against: a transition is not
     * an outcome. Arrival is only counted after the request has gone out. */
    if (g_console_join_asked && lsw2_state_current() == kLsw2BrowseState) {
        if (g_console_join_pending) {
            g_console_join_pending = false;
            std::fprintf(stderr,
                         "[fork] console Join: the cartridge's own wireless "
                         "browse list (state %u) is OPEN - reached because the "
                         "player chose it at the console entity\n",
                         unsigned(kLsw2BrowseState));
        }
        return true;
    }

    /* THE SESSION HAS TO EXIST, and the POINTER is what says so.
     *
     * Measured: the browse list was entered with vm=00000000 in the log, then
     * the radio came up afterwards. The gate above tests what
     * lsw2_wh_make_view_manager RETURNED, and "already"/"made" are answers
     * about the request, not about the object. lsw2_wh_have_view_manager reads
     * the pointer, which is the fact that matters - a list with no session
     * behind it has nothing to browse. */
    if (!lsw2_wh_have_view_manager()) return true;

    /* LET THE CONSOLE'S OWN REQUEST LAND FIRST.
     *
     * The write watch named the thing that was closing the browse list, and it
     * was not a fault: interacting with the console entity makes the CARTRIDGE
     * ask for the arena lobby state itself, and that request was still in
     * flight when this one went out. The list opened, the cartridge's own 48
     * arrived a moment later, and the list closed - which looked like the list
     * rejecting us and was really us jumping the queue.
     *
     * The host's route never showed this because its 48 lands before Start Game
     * is ever chosen: its trail reads 15 -> 48 -> 54, while the joiner's read
     * 15 -> 34 -> 48 - the joiner had not reached the lobby state at all yet.
     *
     * So wait until the machine is actually sitting in the arena lobby. That is
     * the state a player who walked to the console is in, which makes this a
     * fidelity gate as much as a timing one. */
    if (lsw2_state_current() != kLsw2ArenaLobbyState) {
        static bool waited = false;
        if (!waited) {
            waited = true;
            std::fprintf(stderr,
                         "[fork] console Join: waiting for the console's own "
                         "arena-lobby request to land (state %d, want %d) - "
                         "asking over the top of it is what was closing the "
                         "list\n",
                         lsw2_state_current(), kLsw2ArenaLobbyState);
        }
        return true;
    }

    /* ASK ONCE, THEN LEAVE THE MACHINE ALONE.
     *
     * This is the bug the write watch caught, and it was this file's own: the latch made
     * the tick call this every frame, the printf was guarded but the request
     * was not, and the log shows eight identical requests for state 34 posted
     * back to back. Posting a fresh request every frame into a state machine
     * that is already building one is a known way to crash the guest,
     * and it is also why the list behaved erratically.
     *
     * The pacing is the one lsw2_wh_probe_chooser already proved: a gap
     * between asks, and a bounded number of them, so a request that is simply
     * slow gets a second chance without the machine being flooded. */
    if (g_console_join_gap > 0) { --g_console_join_gap; return true; }
    if (g_console_join_asks >= kConsoleMaxAsks) {
        if (g_console_join_pending) {
            g_console_join_pending = false;
            std::fprintf(stderr,
                         "[fork] console Join: gave up after %d requests for "
                         "state %u - the list is not opening, and repeating "
                         "the ask forever would only hide that\n",
                         kConsoleMaxAsks, unsigned(kLsw2BrowseState));
        }
        return true;
    }

    /* AN EXPERIMENT, off by default and named as one.
     *
     * MEASURED FACT this is testing against: reading the child's WH context
     * over the debug surface showed the scan HAS ALREADY FOUND THE HOST - the
     * result buffer at 0x0210B460 holds the beacon, "NDSR" and "ndsrecomp"
     * legible in it - while the browse list on screen stays empty. So the
     * radio, the transport and the scan all work, and what fails is the list
     * showing what was found.
     *
     * THE SUSPICION: the session bringup starts a scan of its own, and this
     * function then asks for a FRESH state 34 on top of it. A list that did not
     * start the scan it is meant to display may simply have nothing wired to
     * it. LSW2_JOIN_NO_REQUEST=1 skips the request and lets the cartridge's own
     * flow arrive wherever it arrives, which distinguishes those two cases in
     * one run. */
    static const bool no_request = [] {
        const char* e = std::getenv("LSW2_JOIN_NO_REQUEST");
        return e && *e == '1';
    }();
    if (no_request) {
        if (g_console_join_pending) {
            g_console_join_pending = false;
            std::fprintf(stderr,
                         "[fork] console Join: LSW2_JOIN_NO_REQUEST - NOT "
                         "asking for state %u; letting the cartridge's own "
                         "wireless flow land where it lands\n",
                         unsigned(kLsw2BrowseState));
        }
        return true;
    }

    if (!g_console_join_asked) {
        g_console_join_asked = true;
        std::fprintf(stderr,
                     "[fork] console Join: opening the cartridge's own wireless "
                     "browse list (state %u) - the player chose this from the "
                     "console entity, not the fork's boot probe\n",
                     unsigned(kLsw2BrowseState));
    }

    ++g_console_join_asks;
    g_console_join_gap = kConsoleAskGap;
    lsw2_guest_call(lsw2_arm9_FUN_0205ecc4, kLsw2StateRequester,
                    kLsw2StateManager, uint32_t(kLsw2BrowseState), 1u, 1u, 2,
                    0u, 0u);
    return true;
}

void lsw2_wh_console_start_tick() {
    /* SERVICE THE LATCHED PRESS. lsw2_fork_begin_round calls the route once,
     * because take_start_request() yields the press once; if the session was
     * still coming up at that moment the rest of the work has to happen on
     * later frames, and this is what gives it them. Silent and free when the
     * host has not pressed Start Game. */
    if (g_console_start_pending) lsw2_wh_console_start();
    if (g_console_join_pending) lsw2_wh_console_join();
}

/* -- Arena multiplayer / the join prompt, auto-answered ----------------------
 *
 * Since the real-WH session became the default, a joining peer brings the
 * HOST to state 35 - the cartridge's own "let them join? Yes/No" - and
 * nothing was answering it. The downstream symptoms were exactly what was
 * observed: the vanilla prompt shown; if the
 * loading chain then runs without a genuine admission the host stalls to a
 * black screen; and because the WM view-mask already counts 2 with
 * approval outstanding, the lobby countdown advances while neither
 * player sees the other and the signal meter stays red. It was proven that
 * answering YES (Left selects Yes, A confirms) completes the real connection
 * and lands both consoles in kLsw2ArenaLobbyState.
 *
 * This drives that answer automatically. A tiny frame-driven sequence presses
 * Left (move the cursor onto Yes - a no-op if it is already there, Yes being
 * leftmost) then A (confirm), with release gaps so each is a fresh EDGE the
 * cartridge's menu registers. It repeats every ~10 frames while the host is
 * still on state 35, so a press missed on one cycle is retried, and stops the
 * instant the state leaves 35. Host + fork + real-WH only. */
namespace {
/* DS KEYINPUT bits, the two this needs; naming them here keeps the input
 * vocabulary local rather than pulling the whole menu header in. */
constexpr uint16_t kKeyA = 1u << 0;
constexpr uint16_t kKeyLeft = 1u << 5;
constexpr int kAnswerCycle = 10;   /* frames per Left-then-A attempt */
int g_join_answer_phase = 0;       /* 0 = not answering; >0 = frame within run */
}  // namespace

void lsw2_wh_answer_join_prompt_tick() {
    if (!lsw2_wh_enabled() || !ui_fork_build() || !lsw2_wh_role_is_host()) {
        g_join_answer_phase = 0;
        return;
    }
    if (lsw2_state_current() != kLsw2JoinPromptState) {
        if (g_join_answer_phase != 0) {
            std::fprintf(stderr,
                         "[fork] join prompt: answered - host left state 35 "
                         "(the peer is admitted; both should reach the lobby)\n");
            g_join_answer_phase = 0;
        }
        return;
    }
    if (g_join_answer_phase == 0)
        std::fprintf(stderr,
                     "[fork] join prompt (state 35): auto-answering YES so the "
                     "player is never asked and the peer is really admitted\n");
    ++g_join_answer_phase;
    if (g_join_answer_phase >= 100000) g_join_answer_phase = 1;  /* no overflow */
}

uint16_t lsw2_wh_join_answer_keys() {
    if (g_join_answer_phase == 0) return 0;
    switch ((g_join_answer_phase - 1) % kAnswerCycle) {
        case 0: case 1: return kKeyLeft;   /* select Yes (edge) */
        case 4: case 5: return kKeyA;      /* confirm (edge)    */
        default:        return 0;          /* release gaps      */
    }
}

/* -- The stock join screens, not shown ----------------------------------------
 *
 * The stock bottom-screen lobby screens auto-approve and are not shown; only
 * the loading screen is. The flow transitions keep working, but the stock
 * game's manual approval and lobby list are not needed when the fork has its
 * own systems for joining.
 *
 * A fork join still runs the cartridge's wireless screens underneath, because
 * the join IS the cartridge's own handshake, and both are already answered
 * without the player: the prompt above, the browse pick below. What the player
 * still SAW was their text, a second or two each between the lobby and the
 * loading screen (40): the joiner's "Waiting for Host" (34) and the same title
 * over the lobby's names (39), and the host's "<name> is trying to connect. Do
 * you want to let them join your game? Yes No" (35).
 *
 * Measured on the touch screen through a join, from engine B's registers: all
 * of that text is engine B's BG0 (BGCNT 0x1C10) in all three states. The rest
 * of the picture is the loading screen's own: the starfield on BG2 and BG3
 * (0x1E02 and 0x1F01, the two layers state 40 draws its bar over) and the
 * signal icon, one OBJ. So leaving BG0 out while those states are up shows
 * exactly the loading screen's backdrop, and state 40 then adds its bar as it
 * always has. PRESENTATION ONLY (patch 0016): the states run, the answers
 * land, the guest's registers are untouched.
 *
 * Fork only - the baseline's co-op keeps the cartridge's wireless screens. */
namespace {
constexpr uint8_t kStockJoinText = 1u << 0;   /* engine B's BG0 */
constexpr int kBottomScreen = 1;
int g_hidden_engine = -1;                      /* where the mask was set */
}  // namespace

void lsw2_wh_hide_stock_join_screens_tick() {
    const int state = lsw2_state_current();
    const bool stock = ui_fork_build() &&
                       (state == kLsw2BrowseState || state == kLsw2JoinPromptState ||
                        state == kLsw2JoinWaitState);
    const int engine = stock ? lsw2_screen_engine(kBottomScreen) : -1;
    if (engine == g_hidden_engine) return;
    /* Through the one keeper of the mask (hiddenbgs.h): the match HUD's starfield uses
     * the same engine's mask in a match. */
    lsw2_hidden_bgs_set(Lsw2HiddenBgOwner::StockJoinText, engine, kStockJoinText);
    std::fprintf(stderr, "[fork] stock join screen: %s (state %d)\n",
                 engine >= 0 ? "its text is not shown - the loading screen's "
                               "starfield is"
                             : "back to the whole picture",
                 state);
    g_hidden_engine = engine;
}

/* -- Four-player sessions / keep the host accepting so 3rd & 4th players can join -
 *
 * THE FOUR-PLAYER BLOCKER, PROVEN BY READING. The host sets up a real
 * four-player session - WMParentParam.maxEntry (0x0210B3D2) reads 7 at runtime -
 * but once the FIRST child associates and the host starts data-sharing, the host's
 * beacon "accept" byte clears: child2's scanned-beacon buffer (0x0210B460 on the
 * joiner) holds the host name "Maclunkey" yet accept(+0x4B)=0, curEntry(+0x43)=1.
 * A second child receives that beacon, correctly refuses a non-accepting host, and
 * never leaves WH SCANNING. An earlier maxEntry=1 theory was a wrong inference; the
 * cap is 7 and the AID bitmap at WM-work+0x182 can hold that many, so the fix is
 * simply to KEEP the parent accepting after the first child.
 *
 * WM_SetEntry (FUN_020d8748: FIFO cmd 0x19, arg `enable` < 2) is the lever. We
 * re-issue WM_SetEntry(1) periodically while the host is in WH DATASHARING - the
 * exact state in which entry has closed. Its own precheck (FUN_020d65d0, wants WM
 * state >= 2) makes an early call a harmless no-op, and the periodic cadence keeps
 * the FIFO from being flooded. An external poke cannot do this: the debug surface
 * caps ~28 acked writes/s, far too slow to hold a per-frame-cleared flag, so the
 * re-open must run in-guest. Gated behind LSW2_KEEP_ENTRY until proven,
 * to protect the committed two-player match. */
namespace {
/* WM_SetEntry: FUN_020d8870(callback, enable, ...) - FIFO cmd 0x21, valid in WM
 * library parent states {2,7,9}. The host sits in WM-lib state 9 (measured)
 * throughout the lobby, so this is the call whose precheck passes; the earlier
 * cmd-0x19 attempt (FUN_020d8748) fires in any state but is SetBeaconIndication and
 * did nothing. Nitro parents auto-clear entryFlag after each child connects, so the
 * game must re-issue WM_SetEntry(TRUE) to admit the next - which the fork's driven
 * lobby never does. */
constexpr uint32_t kWmSetEntry = 0x020d8870u;  /* WM_SetEntry(callback, enable)   */
constexpr int kWhDataSharing = 5;              /* WH_SYSSTATE_DATASHARING          */
/* DELIVERED FROM WITHIN GUEST EXECUTION. Re-opening entry from the fork
 * tick - which runs BETWEEN frames with no live scheduler - sent the WM FIFO
 * command at a bad point in the ARM7 MP cycle and dropped the FIRST child after
 * enough calls (child1 collapsed even before child2 connected). A one-shot kept
 * child1 alive but the entryFlag re-closed before child2's scan caught it. So this
 * runs from netgame_tick_probe (a per-frame dispatch override, scheduler live) and
 * re-opens frequently: WM_SetEntry is non-blocking, so calling it there is safe and
 * does not disturb the established peer, while a fast cadence keeps the room open
 * across child2's scan window. */
constexpr int kKeepEntryWarmup = 30;           /* let child1 settle ~0.5s first     */
constexpr int kKeepEntryPeriod = 15;           /* re-open ~4x/s to catch child2      */
constexpr int kKeepEntrySettle = 150;          /* after a child associates, pause ~2.5s */
/* The WM work-area station bitmap (work+0x182): bit N set = AID N is connected.
 * WM ctx = *(0x02117E24); work = *(ctx+4). Returns 0 if the pointers are not up. */
uint16_t lsw2_wm_aid_bitmap() {
    const uint32_t ctx = bus_device_read32(0, 0x02117E24u);
    if (ctx < 0x02000000u || ctx >= 0x02400000u) return 0;
    const uint32_t work = bus_device_read32(0, ctx + 4u);
    if (work < 0x02000000u || work >= 0x02400000u) return 0;
    const uint32_t w = bus_device_read32(0, (work + 0x182u) & ~3u);
    return uint16_t((w >> (((work + 0x182u) & 2u) * 8u)) & 0xFFFFu);
}
bool keep_entry_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_KEEP_ENTRY");
        return e && *e == '1';
    }();
    return on;
}
/* EXPERIMENT (LSW2_MP_REFORM): when a 2nd child (AID 2) appears in the WM
 * work-area station bitmap, the host reacts by reloading and drops child1. This
 * tries to explicitly RE-FORM the MP group once for the expanded member set by
 * guest-calling the host's MP-start handler FUN_020ae4d4 (which calls WM_StartMP),
 * to see whether an explicit re-form integrates the 2nd child instead of collapsing
 * the first. Off by default. */
bool mp_reform_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_MP_REFORM");
        return e && *e == '1';
    }();
    return on;
}
/* RETRACTED: LSW2_MP_ADD is gone. It guest-called
 * FUN_020d75d8(0, aid) believing it to be a "WM MP-member add" (FIFO cmd 0xD). It is
 * WM_Disconnect(callback, aid) - NitroSDK's WM API id 0xD - and FUN_020ae1d0, read in
 * full, is wh.c's WH_StateOutStartParent: on WM_STATECODE_CONNECTED it sets the
 * connected bit when sJudgeAcceptFunc (WHCTX+0x10) is NULL or approves, and calls
 * WM_Disconnect only when a judge REJECTS. WH_Initialize stores NULL there
 * (0x020AD504) and nothing in the image ever stores a judge, so the game accepts
 * every child - and the experiment, when enabled, disconnected the very child it
 * meant to seat. */
int g_keep_entry_phase = 0;
bool g_mp_reformed = false;
int g_ke_settle_until = -1;       /* phase until which re-opening is paused (a child settling) */
uint16_t g_ke_last_bitmap = 0;    /* last-seen WM AID bitmap, to detect a new association */
}  // namespace

void lsw2_wh_keep_entry_tick() {
    if (!lsw2_wh_enabled() || !ui_fork_build() || !lsw2_wh_role_is_host()) {
        g_keep_entry_phase = 0;
        return;
    }
    if (!keep_entry_enabled()) return;
    /* Only while the host has a live parent session that has closed to new
     * children - WH DATASHARING. Before that, entry is already open. */
    if (lsw2_wh_state() != kWhDataSharing) {
        g_keep_entry_phase = 0;
        g_mp_reformed = false;   /* re-arm the reform for the next session */
        g_ke_settle_until = -1;
        g_ke_last_bitmap = 0;
        return;
    }
    if (!lsw2_guest_ready()) return;
    /* CRITICAL (found in a live match): the ARENA (game state 40/41/42)
     * is ALSO WH DATASHARING, so gating only on WH==DATASHARING kept firing WM FIFO
     * commands (WM_SetEntry / the MP-add / the reform) DURING arena rendering, which
     * wedged the GX geometry engine (ARM9 halt, "scheduler wedge", host stuck black).
     * Children are only admitted in the JOINABLE LOBBY, so fire the WM guest-calls
     * ONLY when the GAME state is a lobby state (48 = arena lobby, or the host menu
     * 54); once the match starts, stop entirely. This both fixes the wedge and is the
     * correct batch-association window (seat all children before the arena starts). */
    {
        const int gs = lsw2_state_current();
        if (gs != kLsw2ArenaLobbyState && gs != int(host_menu_state())) {
            if (g_keep_entry_phase != 0)
                std::fprintf(stderr,
                             "[fork] keep-entry: game state %d is not a lobby (48/%u) - "
                             "stopping WM guest-calls so the arena is not GX-wedged"
                             "\n", gs, unsigned(host_menu_state()));
            g_keep_entry_phase = 0;   /* re-arm warmup if we return to the lobby */
            return;
        }
    }
    if (g_keep_entry_phase == 0)
        std::fprintf(stderr,
                     "[fork] keep-entry: host in DATASHARING - re-opening the room "
                     "(WM_SetEntry(1), cmd 0x21) ~%dx/s from within the netgame tick "
                     "so a 2nd/3rd/4th child can associate (maxEntry=7, state 9"
                     ")\n",
                     60 / kKeepEntryPeriod);
    /* AUTO-BACKOFF: when a NEW child associates (a new bit appears in the
     * WM AID bitmap), PAUSE re-opening for kKeepEntrySettle frames so its just-formed
     * association settles. Re-issuing WM_SetEntry over the top of a mid/just-associated
     * child was dropping child2 (DATASHARING->STOP). Resume after the settle window to
     * admit the next child, so 3rd/4th still get in - one at a time. */
    const uint16_t bitmap = lsw2_wm_aid_bitmap();
    if (bitmap != g_ke_last_bitmap) {
        if (bitmap & (uint16_t)~g_ke_last_bitmap) {   /* a bit was ADDED = a child joined */
            g_ke_settle_until = g_keep_entry_phase + kKeepEntrySettle;
            std::fprintf(stderr,
                         "[fork] keep-entry: new association (AID bitmap %04X) - backing "
                         "off re-open ~%dms to let it settle\n",
                         bitmap, kKeepEntrySettle * 1000 / 60);
        }
        g_ke_last_bitmap = bitmap;
    }
    const bool settling = (g_keep_entry_phase < g_ke_settle_until);
    if (!settling && g_keep_entry_phase >= kKeepEntryWarmup &&
        ((g_keep_entry_phase - kKeepEntryWarmup) % kKeepEntryPeriod) == 0)
        lsw2_guest_call(lsw2_arm9_FUN_020d8870, kWmSetEntry, 0u /*callback*/,
                        1u /*enable*/, 0u /*param_3*/);
    /* EXPERIMENT (LSW2_MP_REFORM, default off): re-form the MP group once
     * when AID 2 appears. RETAINED for reference but its WM re-init causes a state-38
     * the tolerance cannot catch (regressed the 3-station lobby); the auto-backoff
     * above is the preferred integration path. */
    if (mp_reform_enabled() && !g_mp_reformed && (bitmap & (1u << 2))) {
        g_mp_reformed = true;
        std::fprintf(stderr,
                     "[fork] mp-reform: AID2 present (bitmap %04X) - re-forming MP via "
                     "FUN_020ae4d4\n", bitmap);
        lsw2_guest_call(lsw2_arm9_FUN_020ae4d4, 0x020ae4d4u, 0u);
    }
    ++g_keep_entry_phase;
    if (g_keep_entry_phase >= 100000) g_keep_entry_phase = kKeepEntryWarmup;
}

/* Four-player sessions / suppress the host's TRANSIENT state-38 during a 2nd child's join.
 *
 * The netgame tick FUN_020b07e8 requests state 38 (via a bl 0x020b04f0 at 0x020B08E8)
 * when the player count CHANGED and the view-manager counter reads 0. When
 * child2 joins, the host's count churns to 0 for a few frames -> the host requests 38 ->
 * that teardown churns the host's WH and DROPS child2 (child1 survives via the MP-reform).
 * This lets the host TOLERATE that transient count-0: a post-lift patch guards the
 * bl-0x020b04f0 with `if (!lsw2_fork_suppress_net38())`, and this returns true for a bounded
 * window (kNet38Budget frames) after the reform fires (i.e. after child2 is detected), on
 * the HOST only. When suppressed, the lifted call is skipped and the tick early-returns that
 * frame (the existing R15-mismatch/cancel path), giving the count time to recover without a
 * teardown. Bounded so a genuinely empty session still reaches 38 after the window. Gated
 * behind LSW2_TICK_TOLERATE; default off, so 2-player is unchanged. */
namespace {
/* Frames of a CONSECUTIVE count-0 to tolerate before letting the tick request 38.
 * A 2nd/3rd child's join churns the host's view-manager count to 0 for a handful
 * of frames while it integrates; a genuinely empty session stays 0. ~120 (~2s) is
 * long enough for a join to settle, short enough that a real end still reaches 38. */
constexpr int kNet38Tolerate = 120;
bool tick_tolerate_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_TICK_TOLERATE");
        return e && *e == '1';
    }();
    return on;
}
int  g_net38_streak = 0;       /* consecutive frames the 38-request condition held */
bool g_net38_hit_frame = false;/* set by the guard when the 38-request site fires   */
}  // namespace

/* CORRECTED: the guard is reached ONLY when FUN_020b07e8 is about to
 * request state 38 because the player count CHANGED and the view-manager counter
 * reads 0 (the count churned to 0). The prior version gated on g_mp_reformed and so
 * NEVER fired (the 38 fires BEFORE the reform latches). This tolerates a TRANSIENT
 * count-0: suppress while the consecutive-count-0 streak is short (a join settling),
 * allow the 38 once it persists (a real empty session). Streak advanced per-frame by
 * lsw2_wh_net38_frame_end(). No dependency on the MP-reform band-aid. Host + fork
 * only, gated LSW2_TICK_TOLERATE (default off -> 2-player unchanged). */
extern "C" int lsw2_fork_suppress_net38(void) {
    if (!tick_tolerate_enabled() || !ui_fork_build() || !lsw2_wh_role_is_host())
        return 0;
    g_net38_hit_frame = true;
    if (g_net38_streak < kNet38Tolerate) {
        if (g_net38_streak == 0)
            std::fprintf(stderr,
                         "[fork] net38-tolerate: transient count-0 (a join settling) - "
                         "suppressing the host's state-38 for up to %d frames\n",
                         kNet38Tolerate);
        return 1;   /* suppress: the tick cancels this frame, count can recover */
    }
    return 0;       /* count-0 persisted -> a real empty session, let 38 fire */
}

/* Per-frame from netgame_tick_probe AFTER the lifted tick: advance the consecutive
 * count-0 streak, or reset it when the tick did NOT request 38 (the count recovered). */
void lsw2_wh_net38_frame_end(void) {
    if (g_net38_hit_frame) ++g_net38_streak;
    else                   g_net38_streak = 0;
    g_net38_hit_frame = false;
}

/* -- Arena multiplayer / the joiner picks the host, auto-selected --------------
 *
 * The mirror of the host's join-prompt auto-answer, one screen earlier and on the
 * other console. The child's WM scan DOES find the host - commit 4e70f340 read the
 * found-parent table live: 16 entries of 0xC4 bytes at 0x0210BF40, an entry
 * counted when the word at entry+0xC0 is non-zero, and entry 0 held the host's MAC
 * and UTF-16 name. What that same commit found is that a headless join never
 * pressed the right keys: the list's SELECTION at [list+0x94] starts at
 * 0xFFFFFFFF (nothing selected), which is why a bare A did nothing - "pressing
 * Down then Up moved the selection to row 0 and A completed the join". So this
 * drives exactly that once a host is present: Down, Up, A on a cycle. It fires
 * only when the found-parent table has a live entry (never presses an empty list,
 * which would back out to the lobby) and stops the instant the radio leaves
 * SCANNING (the connect has begun) or the list closes. Joiner + fork + real-WH.
 *
 * (Earlier passes read 0x0210B460 - the WRONG address - saw all-zero and wrongly
 * concluded the scan was broken; the table is at 0x0210BF40, per 4e70f340.) */
namespace {
constexpr int kBrowsePickCycle = 30;    /* frames per Down-Up-A attempt        */
constexpr int kWhScanning = 2;          /* WH_SYSSTATE_SCANNING                */
constexpr uint16_t kKeyDown = 1u << 7;  /* DS KEYINPUT: Down                   */
constexpr uint16_t kKeyUp = 1u << 6;    /* DS KEYINPUT: Up                     */
/* The found-parent table is coophosts.h's kLsw2HostTable (0x0210BF40) - the same
 * 16 x 0xC4 table the cartridge's row count walks, an entry counted live when its
 * +kLsw2HostTtl word is non-zero (4e70f340). */
int g_browse_pick_phase = 0;            /* 0 = not picking; >0 = frame in run   */

/* ONE-SHOT: does the host's beacon appear ANYWHERE in the child's main RAM?
 * If the WM scan received and stored the parent, the beacon's UTF-16 name is in
 * RAM even if the browse table (0x0210BF40) is empty - which would localise the
 * break to the WH->browse-table copy rather than the scan itself. If it is
 * nowhere, the WM dropped the beacon before storing it. Searches for the UTF-16
 * pattern "Macl" (the host nickname's first 4 chars, from the captured beacon)
 * across 0x02000000..0x02400000. Runs once; caps logged hits. */
void hunt_host_beacon_once() {
    static bool done = false;
    if (done) return;
    done = true;
    /* "M\0a\0c\0l\0" little-endian UTF-16 - two words. */
    const uint32_t w0 = 0x0061004Du;  /* 'M',0,'a',0 */
    const uint32_t w1 = 0x006C0063u;  /* 'c',0,'l',0 */
    int hits = 0;
    for (uint32_t a = 0x02000000u; a < 0x02400000u && hits < 8; a += 4u) {
        if (read32(a) == w0 && read32(a + 4u) == w1) {
            ++hits;
            std::fprintf(stderr,
                         "[fork] beacon hunt: host name (UTF-16 'Macl') at %08X\n",
                         unsigned(a));
        }
    }
    std::fprintf(stderr,
                 "[fork] beacon hunt: %d hit(s) of the host name in main RAM "
                 "(0 => the WM never stored the beacon; >0 => stored, so the "
                 "WH->browse-table copy is the break)\n",
                 hits);
    /* The row-count getter FUN_020AD8E8 reads the table base from the pointer
     * stored at 0x020AD914; read it live so we know WHICH table it counts, and
     * compare the two candidate bases' entry-0 name(+0x50)/ttl(+0xC0). The host
     * name landed at 0x0210B4B0 = 0x0210B460 + 0x50, so 0x0210B460 looks like the
     * REAL found-parent base while the getter may be reading 0x0210BF40. */
    const uint32_t getter_base = read32(0x020AD914u);
    std::fprintf(stderr,
                 "[fork] beacon hunt: getter table base *(0x020AD914)=%08X | "
                 "0x0210B460 name+50=%08X ttl+C0=%08X | 0x0210BF40 name+50=%08X "
                 "ttl+C0=%08X\n",
                 unsigned(getter_base),
                 unsigned(read32(0x0210B460u + 0x50u)),
                 unsigned(read32(0x0210B460u + 0xC0u)),
                 unsigned(read32(0x0210BF40u + 0x50u)),
                 unsigned(read32(0x0210BF40u + 0xC0u)));
    /* Find the host MAC OUI 00:09:BF in the WM buffer region and dump context, so
     * a real WMBssDesc (BSSID + name together) is located and its BSSID offset
     * relative to the name becomes legible - the field the connect needs. */
    auto rb = [](uint32_t a) -> uint8_t {
        return uint8_t((read32(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu);
    };
    int machits = 0;
    for (uint32_t a = 0x02100000u; a < 0x02120000u && machits < 6; ++a) {
        if (rb(a) == 0x00 && rb(a + 1) == 0x09 && rb(a + 2) == 0xBF) {
            ++machits;
            char line[160]; int lp = 0;
            for (uint32_t w = 0; w < 0x30u; w += 4u)
                lp += std::snprintf(line + lp, sizeof(line) - lp, "%08X ",
                                    unsigned(read32((a & ~3u) + w)));
            std::fprintf(stderr, "[fork] MAC OUI 00:09:BF at %08X ctx: %s\n",
                         unsigned(a), line);
        }
    }
    std::fprintf(stderr, "[fork] beacon hunt: %d MAC-OUI hit(s) in 0x02100000-20000\n",
                 machits);
    /* The child's SCAN BLOCK is at 0x0210B380 - in 4e70f340 it targeted
     * channel 7 with a broadcast BSSID. And the WH context +0x00=role, +0x02=chan,
     * +0x20=state. Dump both live to compare - a wrong scan channel/BSSID would
     * explain why the WM never commits the host to 0x0210BF40 in the current build. */
    std::fprintf(stderr, "[fork] scan block 0x0210B380:");
    for (uint32_t w = 0; w < 0x40u; w += 4u)
        std::fprintf(stderr, " %08X", unsigned(read32(0x0210B380u + w)));
    std::fprintf(stderr, "\n");
    /* The child's scan CALLBACK lives at WH-ctx+0x34 (base 0x0210B320)
     * and should be 0x020ADCA4 (FUN_020adca4, the commit that copies a found
     * beacon into the found-parent table DAT_020add68 and sets its TTL). If the
     * callback is null/wrong, the WM never commits -> display table empty. And the
     * commit table base *(0x020ADD68) must equal the getter's base 0x0210BF40, or
     * the commit writes a table the getter never reads. */
    std::fprintf(stderr,
                 "[fork] WH-ctx+0x34 callback=%08X (want 020ADCA4) | commit-table "
                 "*(0x020ADD68)=%08X (want 0210BF40) | WH role+0x00=%08X state+0x20=%08X\n",
                 unsigned(read32(0x0210B320u + 0x34u)),
                 unsigned(read32(0x020ADD68u)),
                 unsigned(read32(0x0210B320u)),
                 unsigned(read32(0x0210B320u + 0x20u)));
}

/* EXPERIMENT (LSW2_FORK_COMMIT): the WM scan receives the beacon but never
 * commits a found-parent into the display table 0x0210BF40, so the browse list is
 * empty. This does the commit the fork's way - it writes a joinable row 0 into the
 * display table each frame (status avoiding &3==2, a name, a live TTL), the same
 * fields coophosts.h documents the list reading. If the browse UI then shows the
 * row and the auto-select fires, the display+select path is proven; what the
 * connect (0x020ADBCC) then does with a fork-written record isolates whether it
 * needs a fuller WMBssDesc than these three fields. Off unless the env is set. */
bool fork_commit_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_FORK_COMMIT");
        return e && *e == '1';
    }();
    return on;
}

constexpr uint32_t kLsw2ScanResultBuf = kLsw2WhBssDesc;  /* scan block's result */
constexpr uint32_t kLsw2ScanCallback  = 0x020ADCA4u;  /* WH-ctx+0x34 commit cb    */

/* Do the commit the WM is NOT invoking: call the cartridge's OWN scan callback
 * (FUN_020adca4) with the scan-result buffer as the record. That callback matches
 * the record against the found-parent table by BSSID and, if new, copies its 0xC0
 * bytes into an empty slot at 0x0210BF40 and sets the TTL - exactly the commit
 * that populates the display table with REAL beacon data. We fire it only when the
 * result buffer holds the HOST's record (its name field at +0x50 begins "Ma" in
 * UTF-16), never the framework WifiAP beacon, and only on the guest thread. */
void fork_commit_row0() {
    if (!lsw2_guest_ready()) return;
    if (read32(kLsw2ScanResultBuf + kLsw2HostName) != 0x0061004Du) return;
    lsw2_guest_call(lsw2_arm9_FUN_020adca4, kLsw2ScanCallback, kLsw2ScanResultBuf);
}

/* How many of the found-parent slots are live (entry TTL != 0) - the same test
 * the cartridge's own row-count getter (FUN_020AD8E8, WH vtable +0x5C) applies.
 * Non-zero means the list has a host to pick. */
int scan_found_count() {
    int n = 0;
    for (uint32_t i = 0; i < uint32_t(kLsw2HostSlots); ++i)
        if (read32(kLsw2HostTable + i * kLsw2HostStride + kLsw2HostTtl) != 0u)
            ++n;
    return n;
}

void dump_scan_result_once() {
    static bool done = false;
    if (done) return;
    done = true;
    char ascii[65];
    for (int i = 0; i < 64; ++i) {
        const uint32_t w = read32((kLsw2HostTable + uint32_t(i & ~3)));
        const uint8_t b = uint8_t((w >> ((i & 3) * 8)) & 0xFFu);
        ascii[i] = (b >= 0x20 && b < 0x7F) ? char(b) : '.';
    }
    ascii[64] = 0;
    std::fprintf(stderr,
                 "[fork] browse pick: found-parent table %08X count=%d ascii=\"%s\""
                 " (e0 ttl=%08X)\n",
                 unsigned(kLsw2HostTable), scan_found_count(), ascii,
                 unsigned(read32(kLsw2HostTable + kLsw2HostTtl)));
}
}  // namespace

void lsw2_wh_pick_browse_host_tick() {
    if (!lsw2_wh_enabled() || !ui_fork_build() || lsw2_wh_role_is_host()) {
        g_browse_pick_phase = 0;
        return;
    }
    /* WORKAROUND (LSW2_FORCE_WM_ENABLE): an untracked framework change
     * (bus.cpp/runtime_arm.cpp, no git history/backup) stopped the
     * joiner's WM from being ENABLED even though the radio HW powers on ("WIFI:
     * ON" is byte-identical to 0e525aba). The WM-lib enabled flag - the halfword
     * at *(0x020D65CC), which FUN_020d65b4 tests - stays 0, so WM_GetAllowedChannel
     * (FUN_020d6e4c) returns 0x8000, MeasureChannel (FUN_020aeed0) raises error 3,
     * and the joiner never reaches SCANNING. Since the HW is up, force that flag
     * (and the allowed-channel mask at 0x027FFCFA that FUN_020d6e4c returns when the
     * flag is set) non-zero EVERY FRAME for the joiner, so MeasureChannel proceeds
     * and the child scans -> connects (the 0e525aba flow). Every frame because an
     * external poke reverts within a frame. Off unless LSW2_FORCE_WM_ENABLE=1. */
    {
        static const bool force_wm = [] {
            const char* e = std::getenv("LSW2_FORCE_WM_ENABLE");
            return e && *e == '1';
        }();
        /* Force during BUSY(3) or MEASURECHANNEL(7), NOT STOP(0): forcing at STOP
         * breaks init (measured STOP->BUSY err3 step1C), and gating on the transient
         * MEASURECHANNEL(7) never fires (the allowed-channel query errors within the
         * same frame the state becomes 7, before this per-frame tick sees it). So
         * force through the BUSY bring-up so the flag is already set when
         * MeasureChannel's FUN_020d6e4c runs. */
        const int whst_force = lsw2_wh_state();
        if (force_wm && (whst_force == 3 || whst_force == 7)) {
            static int flag_log = 0, mask_log = 0;
            const uint32_t p = read32(0x020D65CCu);   /* WM context pointer */
            if (p >= 0x02000000u && p < 0x03000000u) {
                const uint32_t w = read32(p);          /* flag = low halfword at p */
                if ((w & 0xFFFFu) == 0u) {
                    write32(p, (w & 0xFFFF0000u) | 1u);
                    if (flag_log++ < 1)
                        std::fprintf(stderr, "[fork] FORCE_WM_ENABLE: WM flag at "
                                     "*(0x020D65CC)=%08X was 0 -> forced 1\n",
                                     unsigned(p));
                }
            }
            /* mask halfword at 0x027FFCFA = high halfword of the word at 0x027FFCF8 */
            const uint32_t mw = read32(0x027FFCF8u);
            if ((mw >> 16) == 0u) {
                write32(0x027FFCF8u, (mw & 0x0000FFFFu) | (0x7FFEu << 16));
                if (mask_log++ < 1)
                    std::fprintf(stderr, "[fork] FORCE_WM_ENABLE: channel mask at "
                                 "0x027FFCFA was 0 -> forced 0x7FFE\n");
            }
        }
    }
    /* Only while the joiner sits in the browse list WITH the radio scanning. The
     * instant the connect begins (WH leaves SCANNING) or the list closes, stop -
     * a stale press into whatever screen comes next would be a real bug. */
    if (lsw2_state_current() != kLsw2BrowseState ||
        lsw2_wh_state() != kWhScanning) {
        if (g_browse_pick_phase != 0) {
            std::fprintf(stderr,
                         "[fork] browse pick: stopped - state %d, WH %d "
                         "WH_SYSSTATE_%s (the connect began or the list closed)\n",
                         lsw2_state_current(), lsw2_wh_state(),
                         lsw2_wh_state_name(lsw2_wh_state()));
            g_browse_pick_phase = 0;
        }
        return;
    }
    if (g_browse_pick_phase == 0) {
        std::fprintf(stderr,
                     "[fork] browse list (state 34, SCANNING): waiting for a found "
                     "host to auto-select (Down, Up, A) - the game's own join\n");
        dump_scan_result_once();
    }
    ++g_browse_pick_phase;
    /* FULL RECEIVED-BEACON RECORD DUMP for the host-side/child-side diff.
     * Fires EARLY (phase 3) and periodically, on ANY child in the browse: a
     * fresh-host child (curEntry=0) dumps it before it classifies and leaves, and a
     * stuck child2 (curEntry=1) dumps it throughout. Diffing the two records shows
     * which game-info field (accept flag / curEntry) changes when the host already
     * has a child - splitting host-side maxEntry from child-side classification. */
    if (g_browse_pick_phase == 3 || (g_browse_pick_phase % 20) == 0) {
        std::fprintf(stderr, "[fork] beacon record 0x0210B460 [phase %d]:",
                     g_browse_pick_phase);
        for (uint32_t w = 0; w < 0xC4u; w += 4u)
            std::fprintf(stderr, " %08X", unsigned(read32(0x0210B460u + w)));
        std::fprintf(stderr, "\n");
    }
    /* EXPERIMENT: fork-commit a joinable row into the display table so the browse
     * list shows a host, letting the select+connect path run (LSW2_FORK_COMMIT). */
    if (fork_commit_enabled() && (g_browse_pick_phase % 15) == 1) {
        fork_commit_row0();
        if (g_browse_pick_phase == 1)
            std::fprintf(stderr, "[fork] FORK_COMMIT: invoking the cartridge's own "
                                 "scan callback 0x020ADCA4 on the scan-result buffer "
                                 "to commit the host into %08X\n",
                         unsigned(kLsw2HostTable));
    }
    /* A few seconds into the browse list, hunt the whole of main RAM once for the
     * host's beacon name - splits "WM never stored it" from "stored, copy broke". */
    if (g_browse_pick_phase == 200) hunt_host_beacon_once();
    /* Report the found-parent count as it evolves, so a run shows WHETHER and
     * WHEN the WM scan registers the host (the dump above is a one-shot at entry,
     * before the scan has had any time). Logs only when the count changes. */
    {
        static int last_count = -1;
        const int now = scan_found_count();
        if (now != last_count) {
            last_count = now;
            std::fprintf(stderr,
                         "[fork] browse pick: found-parent count -> %d (phase %d) "
                         "e0 ttl=%08X name0=%08X\n",
                         now, g_browse_pick_phase,
                         unsigned(read32(kLsw2HostTable + kLsw2HostTtl)),
                         unsigned(read32(kLsw2HostTable + kLsw2HostName)));
            if (now > 0) {
                /* Dump the committed record's head so its BSSID (the field the
                 * connect targets) is legible - a real host MAC (00:09:BF:..)
                 * means the handshake is the gap; garbage means the callback got
                 * the wrong param buffer. */
                std::fprintf(stderr, "[fork] committed record 0x0210BF40 head:");
                for (uint32_t w = 0; w < 0x18u; w += 4u)
                    std::fprintf(stderr, " %08X",
                                 unsigned(read32(kLsw2HostTable + w)));
                std::fprintf(stderr, "\n");
            }
        }
    }
    if (g_browse_pick_phase >= 100000) g_browse_pick_phase = 1;
}

uint16_t lsw2_wh_browse_pick_keys() {
    if (g_browse_pick_phase == 0) return 0;
    /* DO NOT PRESS AN EMPTY LIST - pressing when nothing is found backs the child
     * out to the arena lobby. Fire only once the found-parent table has a live
     * entry (the same non-zero-at-+0xC0 test the cartridge's row count uses). */
    if (scan_found_count() == 0) return 0;
    /* The join the manual route proved (4e70f340): Down then Up moves the
     * selection from -1 onto row 0, then A completes the join. Each is a fresh
     * edge with release gaps so the menu registers a press, retried on a cycle
     * until the radio leaves SCANNING. */
    switch (g_browse_pick_phase % kBrowsePickCycle) {
        case 0: case 1:   return kKeyDown;  /* nudge the selection (edge)   */
        case 6: case 7:   return kKeyUp;    /* land on row 0 (edge)         */
        case 12: case 13: return kKeyA;     /* complete the join (edge)     */
        default:          return 0;         /* release gaps                 */
    }
}

bool lsw2_wh_have_view_manager() {
    const uint32_t mgr = read32(kLsw2ViewManagerPtr);
    return mgr >= 0x02000000u && mgr < 0x02400000u;
}

int lsw2_wh_make_view_manager() {
    if (lsw2_wh_have_view_manager()) {
        if (g_tramp == kTrampRan) {
            g_tramp = kTrampDone;
            std::fprintf(stderr,
                         "[fork] view manager: BUILT at %08X by the "
                         "cartridge's own allocator - state 54's virtual call "
                         "at 0x0209E2A0 now has an object to go through\n",
                         unsigned(read32(kLsw2ViewManagerPtr)));
            return kLsw2ViewManagerMade;
        }
        return kLsw2ViewManagerAlready;
    }

    switch (g_tramp) {
    case kTrampIdle:
        g_tramp = kTrampWanted;
        std::fprintf(stderr,
                     "[fork] view manager: none yet - handing the guest's own "
                     "PC to the cartridge's allocator 0x0207281C at the next "
                     "arena-lobby flag test. State 54 cannot run without it.\n");
        return kLsw2ViewManagerNotNow;

    case kTrampWanted:
    case kTrampRunning:
        return kLsw2ViewManagerNotNow;   /* the hop has not happened yet */

    case kTrampRan:
        /* IT RAN AND THERE IS STILL NO MANAGER, which is the quiet failure
         * this has to say out loud. FUN_0207281C guards the constructor with
         * `if (thunk_FUN_0205E540(x, 3, 4))` and stores 0 when that is false,
         * so a completed run with a null pointer means that guard refused. */
        g_tramp = kTrampDone;
        std::fprintf(stderr,
                     "[fork] view manager: the allocator RAN TO COMPLETION on "
                     "the guest's thread and the pointer is STILL 00000000 - "
                     "FUN_0207281C's guard thunk_FUN_0205E540(x,3,4) refused, "
                     "so it stored 0. Nothing may be built on this.\n");
        return kLsw2ViewManagerStillNull;

    case kTrampDone:
    default:
        return kLsw2ViewManagerStillNull;
    }
}

void lsw2_wh_probe_select_wireless() {
    if (!g_probe_asked || g_probe_selected) return;
    if (lsw2_state_current() != 54) return;      /* the chooser */

    const uint32_t mgr = read32(kLsw2StateManager + kLsw2StateCurrent);
    if (mgr < 0x02000000u || mgr >= 0x02400000u) return;

    g_probe_selected = true;

    /* READ ONLY, UNTIL THE FIELD IS FOUND.
     *
     * The first version wrote 1 to +0xEC on the strength of the constructor
     * being decompiled as `obj[0x3b] = 0; obj[0x3c] = 0x39;` with obj an
     * undefined4* - so word 0x3b is byte 0xEC. The live object says otherwise:
     * +0xEC held 0x03E00068 and +0xF0 held 0x0206xxxx, neither of which is the
     * 0 and 0x39 the constructor is supposed to leave. So the write went
     * somewhere else, and a write to an unidentified field of a live object is
     * exactly the kind of thing this project reverts rather than tunes.
     *
     * DUMP AND LOOK. FUN_0208EA58 switches on the mode and case 0 requests the
     * state in the neighbouring word, which the constructor sets to 0x39 - so
     * the pair to find is a 0 immediately followed by a 0x39. That is a
     * distinctive enough signature to identify without guessing again. */
    /* IDENTITY FIRST. +0x00 is the vtable and +0x04 the state id, so if those
     * are not 0x020FF524 and 54 then the object is the wrong one and every
     * offset below is meaningless - which is the mistake the +0xEC write
     * already made once. */
    std::fprintf(stderr,
                 "[fork] PROBE: chooser object %08X  vtable=%08X id=%u%s\n",
                 unsigned(mgr), unsigned(read32(mgr)),
                 unsigned(read32(mgr + 4u) & 0xFFu),
                 read32(mgr) == 0x020FF524u ? "  (state 54's vtable)"
                                            : "  <- NOT state 54's vtable");

    /* THEN HUNT THE CONSTRUCTOR'S SIGNATURE rather than trusting one reading
     * of it. It leaves 0, 5, 0, 0x39 in four adjacent slots; whether those are
     * word indices 0x39..0x3C (bytes 0xE4..0xF0) or byte offsets 0x39..0x3C is
     * exactly what is in doubt, so look for the VALUES. 0x39 is distinctive
     * enough, as a word and as a byte. */
    for (uint32_t off = 0; off < 0x200u; off += 4u) {
        const uint32_t v = read32(mgr + off);
        if (v == 0x39u)
            std::fprintf(stderr, "   word +%03X = 00000039\n", unsigned(off));
        for (int b = 0; b < 4; ++b) {
            if (((v >> (b * 8)) & 0xFFu) == 0x39u &&
                ((v >> (((b + 3) % 4) * 8)) & 0xFFu) == 0x00u)
                std::fprintf(stderr, "   byte +%03X = 39 (word %08X)\n",
                             unsigned(off + uint32_t(b)), unsigned(v));
        }
    }
}

bool lsw2_wh_write_state_request(uint32_t state_id) {
    /* ASK THE STATE MACHINE THE WAY IT ASKS ITSELF, by writing the request.
     *
     * WHY THIS EXISTS RATHER THAN A CALL. Two mechanisms were tried and both
     * failed for structural reasons, not incidental ones:
     *
     *   lsw2_guest_call needs a slice boundary with an empty call-return stack,
     *   and a host with a live netgame essentially never offers one - measured:
     *   "refused by not-ARM9 5139, halted 5095" and the round start
     *   waiting forever.
     *
     *   A trampoline can only act from inside a DISPATCH of its host function,
     *   and the recompiler emits a direct C call whenever the target is in the
     *   same generated bank, so most calls never reach the dispatcher at all.
     *   The hop's host was measured dispatched five times, all at boot.
     *
     * So do what the cartridge does. 0x0205EC4C is the whole of a request:
     * eight words at manager + slot*0x20 + 0x48 - the state id, the mode, the
     * level, an argument, the rest zeroed.
     *
     * RETRACTED: "THE LOCK IT SKIPS". 0x0205ECC4's bracket
     * 0x02001688 / 0x020016C0 is not an interrupt lock - it pushes the manager's
     * current slot and sets it from the requester's slot argument, then pops
     * it. Which is the whole story of the bug below: the slot is an argument.
     *
     * THE SCREEN SLOT, BY NAME - NOT "THE GAME'S OWN CHOICE".
     *
     * This used to write into slot [0x027E02A4], read as "the game's choice"
     * after it was measured at 1 while a lobby was up. It is not a choice, it
     * is the manager's CURRENT slot: the frame loop sets it to each slot in
     * turn (0x0205EDD0, 0x0205EF08) and restores it at 0x02001438, so between
     * frames it is usually 1 - and not always. Measured: opening round two's
     * Game Settings after a late join, it read 0, state 54 landed in the WORLD
     * slot, the lobby level unloaded under the lobby screen, and the fork's own
     * quit rule shut the session down. An earlier finding that "the two
     * requests share one block" was the same bug seen from the score screen. Every caller asks for
     * a screen - 48 or 54 - so the screen slot is named. The second word stays
     * 1: it is the requester's MODE (0x0205EC4C stores r3 there), and 1 is what
     * the cartridge's own route to Game Settings passes (FUN_02072CF4). */
    return lsw2_state_request_in_slot(kLsw2ScreenSlot, state_id, 1u, 0u, 0u);
}

bool lsw2_wh_trampoline_hop(uint32_t host_entry) {
    if (g_tramp == kTrampRunning) {
        /* THE ALLOCATOR HAS COME BACK. Put the host function's world back the
         * way it was - its own return address and the arguments the allocator
         * clobbered - and let it run for real this time. */
        g_tramp = kTrampRan;
        g_cpu.R[14] = g_saved_lr;
        for (int i = 0; i < 4; ++i) g_cpu.R[i] = g_saved_r[i];
        std::fprintf(stderr,
                     "[fork] view manager: the allocator returned on the "
                     "guest's own thread; WH state is %d WH_SYSSTATE_%s\n",
                     lsw2_wh_state(), lsw2_wh_state_name(lsw2_wh_state()));
        return false;
    }

    if (g_tramp != kTrampWanted) return false;

    /* THE WHOLE SESSION START, NOT JUST ITS FIRST STATEMENT.
     *
     * This hop first jumped to FUN_0207281C, the view-manager allocator, on the
     * reasoning that the other three statements of 0x020B10A8 were "about a
     * radio this port does not run". Both halves of that turned out to be
     * wrong, and it was measured:
     *
     *   THE RADIO DOES RUN. With --local-wireless on, the cartridge's own
     *   readiness poll answers honestly - the "answering ready" line never
     *   printed - and the emulated Wi-Fi really does select channels
     *   ("wifi: switching to channel 7"). Nothing needs standing in for.
     *
     *   AND THE DROPPED STATEMENTS ARE THE GAME'S BOOKKEEPING. 0x020B10A8 ends
     *   with `owner[0xC] = 5; owner[6] = 1`, which is how the cartridge records
     *   that a session is up. Running only the allocator brought the radio to
     *   WH_SYSSTATE_MEASURECHANNEL while leaving the game unaware it had a
     *   session at all - so nothing drove it any further, and both consoles sat
     *   in MEASURECHANNEL forever.
     *
     * The old objection to `owner[6] = 1` - that FUN_0208EA58 reads it as
     * "already up" and refuses - is void: FUN_0208EA58 belongs to the class at
     * vtable 0x020FF524, which is NOT this chooser. It was never
     * this state's handler.
     *
     * So run the cartridge's own function, whole, with its own argument. */
    /* A CHILD MUST NOT START A SESSION. In the join role the hop runs only the
     * view-manager allocator - the prerequisite state 0x22 and state 54 share -
     * and stops short of 0x020B10A8, whose whole job is to bring up a session
     * of our own and beacon it. Two parents is the failure that was measured.
     *
     * EXCEPT under LSW2_JOINER_SESSION (an experiment): the joiner NEVER
     * becomes a real netgame participant otherwise - no receive pump, WH stuck
     * at MEASURECHANNEL - so the match cannot complete. This lets the joiner run
     * the FULL session start (0x020B10A8: owner[0xC]=5 DATASHARING, owner[6]=1
     * session-up, the vtable+0x4c call) like the host, to test whether it then
     * runs its pump and reaches DATASHARING, or whether the two-parents
     * deadlock still bites in the current (fork-transport, patch-0012) build. */
    static const bool joiner_session = [] {
        const char* e = std::getenv("LSW2_JOINER_SESSION");
        return e && *e == '1';
    }();
    const bool run_allocator_only =
        (!lsw2_wh_role_is_host() && !joiner_session) ||
        cartridge_menu_starts_session();
    if (run_allocator_only) {
        g_tramp = kTrampRunning;
        g_saved_lr = g_cpu.R[14];
        for (int i = 0; i < 4; ++i) g_saved_r[i] = g_cpu.R[i];
        g_cpu.R[14] = host_entry;
        g_cpu.R[15] = kLsw2ViewManagerAlloc;
        std::fprintf(stderr,
                     "[fork] %s: hop taken to the view-manager allocator "
                     "0x0207281C only - %s\n",
                     lsw2_wh_role_is_host() ? "CARTRIDGE MENU" : "JOIN role",
                     lsw2_wh_role_is_host()
                         ? "state 53's own handler calls the session start"
                         : "a child does not start a session");
        return true;
    }
    if (!lsw2_wh_role_is_host())
        std::fprintf(stderr,
                     "[fork] JOIN role + LSW2_JOINER_SESSION: running the FULL "
                     "session start so the joiner becomes a real netgame "
                     "participant (experiment)\n");

    const uint32_t owner =
        read32(kLsw2WirelessOwner + kLsw2WirelessOwnerField);
    if (owner < 0x02000000u || owner >= 0x02400000u) {
        /* NO OWNER, NO SESSION. Refusing beats jumping into a function whose
         * argument is nonsense - it would write 5 and 1 through a bad pointer.
         * Left in kTrampWanted so a later frame, when the owner exists, tries
         * again. */
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr,
                         "[fork] session start: the wireless owner at %08X+%X "
                         "reads %08X, which is not RAM - waiting\n",
                         unsigned(kLsw2WirelessOwner),
                         unsigned(kLsw2WirelessOwnerField), unsigned(owner));
        }
        return false;
    }

    /* THE HOP. Point the guest at the session start and make it return HERE, so
     * this override runs a second time and the host function is not lost -
     * that is why the host has to be a pure function, and why the one chosen
     * is a bit test with no side effects. */
    g_tramp = kTrampRunning;
    g_saved_lr = g_cpu.R[14];
    for (int i = 0; i < 4; ++i) g_saved_r[i] = g_cpu.R[i];

    g_cpu.R[0] = owner;              /* its one argument */
    g_cpu.R[14] = host_entry;
    g_cpu.R[15] = kLsw2WirelessStart;

    std::fprintf(stderr,
                 "[fork] session start: hop taken - the guest is now inside the "
                 "cartridge's own 0x020B10A8(owner=%08X) and will return to "
                 "%08X\n", unsigned(owner), unsigned(host_entry));
    return true;
}

/* -------------------------------------------- SHUT DOWN LOBBY's SESSION END
 *
 * Shut Down Lobby ends communications and returns the host to the main lobby
 * offline - and until now the fork's version closed only its own TCP
 * session, leaving the cartridge's wireless up.
 *
 * WHY IT IS A HOP AND NOT A CALL. The cartridge's own session end, read
 * whole, reaches FUN_02072bc8, which POLLS WH_End in a do-while until it
 * stops returning zero. Nothing injected synchronously from the host can return
 * from that. It has to run on the guest's thread.
 *
 * WHERE THE HOP IS TAKEN FROM, and why this one can work where an earlier one
 * could not. That attempt rode the arena-lobby flag test, and the last flag
 * test was measured happening at line 175 of a 473-line run - nothing once the
 * lobby settles, so a hop armed by a player's choice would never be taken. This
 * one rides the cartridge's RECEIVE PUMP, which was measured firing once a
 * frame on the guest's thread for as long as two players are connected. A host
 * picking Shut Down Lobby has a peer connected by definition, so the hook is
 * live at exactly the moment the player asks. */
namespace {

enum EndTramp { kEndIdle, kEndWanted, kEndRunning, kEndDone };
EndTramp g_end = kEndIdle;
uint32_t g_end_saved_lr = 0;
uint32_t g_end_saved_r[4] = {0, 0, 0, 0};

/* WHICH HOOK ARMED THE HOP, and this is load-bearing now that there are TWO.
 *
 * The pump serves the host and the netgame tick serves the joiner,
 * and both call the hop with their own address. They share this one state
 * machine, so without recording the armer the other hook would walk into the
 * kEndRunning branch, restore R[14] and r0-r3 that were never saved for it, and
 * corrupt the return of a function it has nothing to do with. The guard below
 * is cheap; the bug it prevents would look like a random wild branch. */
uint32_t g_end_host = 0;

constexpr int kWhStateError = 9;   /* WH_SYSSTATE_ERROR, kStateNames[9] */
/* Set once the tick has had its one look at a broken link. */
bool g_end_gave_way = false;

/* HAS A PEER EVER BEEN SEATED ON THIS CONSOLE, for the joiner watch below.
 * The count starts at 1 and rises, so "it is 1 now" means nothing on its own -
 * only a fall from 2 back to 1 is the host going away. */
bool g_joiner_was_seated = false;

/* Tolerate a TRANSIENT count fall: the host bouncing through a state
 * reset (-1) at char-select entry momentarily drops the seated-peer count 2->1, which
 * looked like the host leaving and made the joiner end its own WH session (WH_End) and
 * then GX-wedge in the char-select scene with a dead session. Only end the session if
 * the count stays below 2 for this many CONSECUTIVE frames (~3s = a real host loss). */
constexpr int kJoinerLossFrames = 180;
int g_joiner_low_streak = 0;

/* HOW LONG THE ARMED RETURN STAYS ARMED, and this is a safety bound rather
 * than a tuning knob.
 *
 * MEASURED, AND IT IS WHY THIS EXISTS. On the HOST the hop returns through the
 * pump and the restore runs. On the JOINER it does not: the teardown
 * nulls the view manager, the netgame tick stops being called at all, and the
 * guest never re-enters the hook that armed the return - measured, "hop taken"
 * with no matching return line, while the console went on running healthily for
 * thirty-four seconds with the view manager confirmed null.
 *
 * LEAVING IT ARMED WOULD BE A LATENT WILD BRANCH. If the tick were ever
 * dispatched again - a later session rebuilding the view manager would do it -
 * the kEndRunning branch would restore an lr and r0-r3 saved for a call that
 * finished long ago, into a completely unrelated one. That is the worst class
 * of bug this project can produce, and "the tick will not be called again" is
 * exactly the kind of assumption that has been wrong four times.
 *
 * So an arm that has not been claimed within two seconds of frames is closed,
 * WITHOUT restoring anything. */
constexpr int kEndReturnDeadline = 120;
int g_end_frames = 0;

}  // namespace

void lsw2_wh_request_session_end(const char* why) {
    if (!lsw2_wh_enabled() || !ui_fork_build()) return;
    /* A LATER SESSION CAN BE ENDED TOO: kEndDone used to
     * be final, so a re-hosted lobby's Shut Down Lobby, or a second Disconnect
     * in the same run, was refused without a line. Re-armed only while the
     * wireless is in a session again (past IDLE) - after an end it sits at
     * IDLE, and a request then (the joiner watch's late one) is still refused. */
    if (g_end == kEndDone && lsw2_wh_state() > 1) g_end = kEndIdle;
    if (g_end != kEndIdle) return;
    g_end = kEndWanted;
    g_end_gave_way = false;
    /* THIS CONSOLE IS LEAVING, SO THE JOINER WATCH IS DISARMED. A count of one
     * after this is its own, not a host that went away. Measured (conn_disc4
     * and conn_disc5): a client that took Disconnect, and one that
     * heard the host close the connection, each ran the cartridge's session
     * end a second time 180 frames later, from their own offline lobby. */
    g_joiner_was_seated = false;
    g_joiner_low_streak = 0;
    /* SAY WHO ASKED. There are two askers now - the host's Shut Down Lobby and
     * the joiner's watch below - and a line that named only the first would be
     * false half the time. This project has been bitten by a log that said the
     * same words for two different outcomes. */
    std::fprintf(stderr,
                 "[fork] %s: asking for the cartridge's own session end on the "
                 "guest's thread at the next dispatched hook\n",
                 why ? why : "session end");
}

bool lsw2_wh_session_end_hop(uint32_t host_entry) {
    if (g_end == kEndRunning) {
        /* ONLY THE HOOK THAT ARMED IT MAY COMPLETE IT. R[14] was pointed at
         * ONE address; the other hook's registers were never saved, so letting
         * it restore them would corrupt a return it knows nothing about. */
        if (host_entry != g_end_host) return false;

        /* IT CAME BACK. R[14] was pointed at the hook, so the cartridge's own
         * end returns here; put the pump's world back before letting it run. */
        g_end = kEndDone;
        g_cpu.R[14] = g_end_saved_lr;
        for (int i = 0; i < 4; ++i) g_cpu.R[i] = g_end_saved_r[i];
        std::fprintf(stderr,
                     "[fork] session end: the cartridge's own 0x020B111C "
                     "RETURNED on the guest's thread; WH state is %d "
                     "WH_SYSSTATE_%s\n",
                     lsw2_wh_state(), lsw2_wh_state_name(lsw2_wh_state()));
        return false;
    }

    if (g_end != kEndWanted) return false;

    /* THE GAME SEES A BROKEN LINK BEFORE THIS ENDS ITS SESSION (found through
     * a crash in a Jabba capture-the-flag match). When the link breaks in a match - the host
     * vanished, WH is in ERROR - the netgame tick (0x020B07E8) has its own
     * answer: it tears the netgame down (0x02072CD0) and asks for world 15 and
     * the communication-error screen 38 (0x020B0B84, 0x020B0BAC). That is the
     * ROM's way out of the arena. This hop is hosted at that tick's entry and
     * at the receive pump's, and whichever came first used to win: when the hop
     * ran first, the teardown below cleared the owner, the tick never reacted,
     * and the client was left in a live arena (conn_halt8, conn_halt9); when
     * the tick ran first, the game left the arena itself and the rest of this
     * ran cleanly after it (conn_halt10). So in a match with the link in
     * ERROR the hop gives way for ONE entry of the tick - the frame the game
     * would have had without the port - and is taken at the next entry either
     * way. */
    if (!g_end_gave_way && lsw2_wh_state() == kWhStateError) {
        const int s = lsw2_state_current();
        if (s == kLsw2MatchState || s == kLsw2PreGameState) {
            if (host_entry == kLsw2NetGameTick) {
                g_end_gave_way = true;
                std::fprintf(stderr,
                             "[fork] session end: the link broke in a match - "
                             "the netgame tick runs first this frame, so the "
                             "game can react to it the way the ROM does\n");
            }
            return false;
        }
    }

    const uint32_t owner =
        read32(kLsw2WirelessOwner + kLsw2WirelessOwnerField);
    if (owner == 0) {
        /* NO SESSION OBJECT, NOTHING TO END - and say so rather than hopping
         * into a function with a null `this`. */
        g_end = kEndDone;
        std::fprintf(stderr,
                     "[fork] session end: REFUSED - the owner word at %08X is "
                     "zero, so the cartridge has no session to end\n",
                     unsigned(kLsw2WirelessOwner + kLsw2WirelessOwnerField));
        return false;
    }

    /* WILL THIS TEARDOWN RETURN THE PLAYER TO A SCREEN, OR ONLY END THE
     * SESSION? Read the byte it decides on, before the hop, so the answer is
     * traced rather than inferred from what happens afterwards.
     *
     * SHUT DOWN LOBBY'S REMAINING HALF. The host was measured surviving the
     * shutdown and ending in the lobby offline, and the joiner surviving it
     * with no communication error - but the joiner then made NO state
     * transition at all, stopping in state 39 rather than returning anywhere.
     *
     * The cartridge's own teardown opens:
     *
     *     020B1128  LDRB r0,[r4,#0x8]      owner[8]
     *     020B1130  CMPNE r1,#0            and the flag argument
     *     020B1134  BEQ  020B1174          either zero -> SKIP the request
     *     020B1168  MOV  r1,#0x4           STATE 4 - the return to a screen
     *     020B1174  BL   02072BC8          the teardown proper, always runs
     *     020B1184  STRB r2,[r4,#0x8]      and then clears owner[8]
     *
     * so a console arriving with owner[8] already zero gets its session torn
     * down and is left exactly where it stood. That is the shape of what the
     * joiner did, and this line is what will confirm or refute it - the flag
     * argument is always 1 from here, so owner[8] is the only variable.
     *
     * owner+8 is word-aligned, so the byte comes out of a 32-bit read rather
     * than needing a read8 this file does not have. */
    const uint32_t owner8 = read32(owner + 8) & 0xFFu;
    std::fprintf(stderr,
                 "[fork] session end: owner %08X has owner[8] = %u, so the "
                 "cartridge's teardown %s request state 4 (the return to a "
                 "screen) before tearing down\n",
                 unsigned(owner), unsigned(owner8),
                 owner8 ? "WILL" : "will NOT");

    /* WHY owner[8] IS LEFT ALONE, having been tried and measured.
     *
     * owner[8] is ZERO on both consoles during a perfectly healthy session -
     * the host hosting with two players seated - so the state-4 request above
     * never happens for anyone. This function has always passed the teardown a
     * flag of 1 precisely to obtain it, and the note explaining why is right
     * about the condition and wrong about the outcome: the flag is only half of
     * `if (owner[8] != 0 && flag != 0)`, and the other half is never true.
     *
     * Setting the byte so the game takes its own branch WORKS MECHANICALLY and
     * is still wrong. Measured: the request fires from the teardown's own call
     * site, and it asks for a level that does not exist -
     *
     *     [state] request 4 from 020B1174  level 74  mode 0x0E CoopEnter
     *
     * 74 is not a stale value. The teardown DERIVES the level from
     * `FUN_020917B4(kLsw2LevelRootAddr)`, which is
     *
     *     LDR    r0,[r0,#0x68]     the level object
     *     CMP    r0,#0
     *     LDRBNE r0,[r0,#0x479]    its index
     *     MOVEQ  r0,#0x4A          NULL -> 74
     *
     * so 74 is the SENTINEL for "there is no level object", and levels.h holds
     * 73 entries. The joiner reaches the teardown sitting on a menu with no
     * level loaded, so the request goes out as "enter level 74 in CoopEnter
     * mode". It took four unmapped ARM9 accesses it had never taken in two
     * prior runs of the identical drive, and did NOT leave state 39 - a fault
     * and no return.
     *
     * SO THE RETURN MUST NAME ITS DESTINATION rather than ask the game where it
     * already is. That needs kLsw2ArenaLobbyLevel and the port's own request
     * path, with the level/state ordering hazard handled deliberately - a real
     * piece of work, not a byte to flip. */

    g_end = kEndRunning;
    g_end_host = host_entry;
    g_end_frames = 0;
    g_end_saved_lr = g_cpu.R[14];
    for (int i = 0; i < 4; ++i) g_end_saved_r[i] = g_cpu.R[i];

    /* ITS TWO ARGUMENTS. `0x020B111C(owner, 1)`, and the second one
     * is load-bearing - the state request that returns the player to a screen
     * is inside `if (owner[8] != 0 && flag != 0)`. Passing 0 would tear the
     * session down and leave the player looking at the lobby they just left. */
    g_cpu.R[0] = owner;
    g_cpu.R[1] = 1;
    g_cpu.R[14] = host_entry;
    g_cpu.R[15] = kLsw2WirelessEnd;

    std::fprintf(stderr,
                 "[fork] session end: hop taken - the guest is now inside the "
                 "cartridge's own 0x020B111C(owner=%08X, 1) and will return to "
                 "%08X\n", unsigned(owner), unsigned(host_entry));
    return true;
}

/* CLOSE AN ARMED RETURN THAT IS NEVER GOING TO BE CLAIMED. See
 * kEndReturnDeadline for the measurement behind this. Runs from the fork's
 * per-frame tick, which keeps running when the hook that armed the hop does
 * not - that is the whole point of putting it there. */
void lsw2_wh_session_end_tick() {
    if (g_end != kEndRunning) return;
    if (++g_end_frames < kEndReturnDeadline) return;

    g_end = kEndDone;
    /* THE STATE IS REPORTED HERE TO SEPARATE TWO EXPLANATIONS. An earlier run
     * saw the joiner stop in state 39 and log no further transition, which reads as "it
     * did not go anywhere" - but it reads exactly the same way if the state
     * DID change and the log simply stopped noticing. Printing the state and
     * its object at the moment the request closes tells those apart, and an
     * absence is only evidence once the instrument is known to be able to see
     * the thing. */
    std::fprintf(stderr,
                 "[fork] session end: the hop armed at %08X was never claimed "
                 "within %d frames, so the guest did not come back through it. "
                 "Closing the request WITHOUT restoring registers - the "
                 "teardown's own effect is what mattered, and restoring an lr "
                 "saved for a finished call into a later one would be a wild "
                 "branch. View manager is now %08X (0 means the teardown "
                 "completed), and the current state is %d at object %08X\n",
                 unsigned(g_end_host), kEndReturnDeadline,
                 unsigned(read32(kLsw2ViewManagerPtr)),
                 lsw2_state_current(),
                 unsigned(lsw2_state_current_object()));
}

/* SHUT DOWN LOBBY'S JOINER HALF, AND THE COMMUNICATION-ERROR CRASH - the
 * console left behind when the host shuts the lobby down.
 *
 * WHAT THE PLAYER SEES WITHOUT THIS: Shut Down Lobby does not gracefully
 * return the players to single player, and a communication error returns the
 * players to the base game's cantina lobby and subsequently crashes. One
 * defect, seen from two directions.
 *
 * WHAT IS ACTUALLY WRONG, read rather than guessed. The
 * joiner IS told the host left - measured: its view mask goes 0x0003 -> 0x0001
 * from the cartridge's own leave store at 020AEAF4, so it correctly ends up
 * holding only itself. The error is what it CONCLUDES. The per-frame netgame
 * tick FUN_020b07e8 sees the count changed, asks the view manager's counter
 * (vtable+0x1C) how many are present, gets zero, and requests state 38 -
 * COMMUNICATION ERROR. It is not a lost peer; it is the cartridge deciding an
 * empty session is a fault.
 *
 * WHY THE HOST DOES NOT HIT IT, which is also the fix. The host runs the
 * cartridge's own session end, and that teardown NULLS the view manager at
 * 0x021092F4. The tick's very first act is `if (view manager == 0)
 * return;` - so on the host the tick goes quiet before it can conclude
 * anything. The joiner never runs that teardown, so its view manager survives
 * and the tick keeps running with nobody to count.
 *
 * SO THE JOINER RUNS THE SAME END THE HOST RUNS. Nothing new is invented - it
 * is the cartridge's own 0x020B111C(owner, 1), the identical call already
 * verified returning cleanly on the host's guest thread.
 *
 * WHY THIS COULD NOT BE BUILT UNTIL NOW. The obvious hook was the receive pump,
 * and it is unavailable at exactly this moment: its guard is
 * `1 < [0x021027F8 + 0x29]` and that byte is the POPCOUNT of the very mask that
 * just dropped to one (viewmask.h). The pump stops being called the instant the
 * joiner needs it. The netgame tick was measured still firing at a player
 * count of ONE, on both consoles, which is what makes this reachable.
 *
 * THE EDGE, NOT THE LEVEL. A count of 1 means nothing on its own - every
 * console boots at 1 and rises. Only a FALL from two back to one is the host
 * going away, so a peer must have been seated first.
 *
 * WHAT "NOT THE HOST" IS ASKED OF. lsw2_wh_role_is_host(), the same predicate
 * that decides which door this console takes into the session at start-up. It
 * is the role, not the fork's own TCP session - asking the fork side would be
 * the wrong-authority mistake this project has made four times. */
void lsw2_wh_joiner_watch(unsigned players) {
    if (!lsw2_wh_enabled() || !ui_fork_build()) return;

    /* THE HOST HAS ITS OWN HALF and it is verified; running both would ask for
     * the same teardown twice. */
    if (lsw2_wh_role_is_host()) return;

    if (players >= 2) {
        g_joiner_was_seated = true;
        g_joiner_low_streak = 0;   /* count is healthy - clear the transient streak */
        return;
    }

    if (!g_joiner_was_seated) return;

    /* EXPERIMENT (LSW2_NO_JOINER_WATCH): in the 3-station test child1's
     * count fell 2->1 the instant child2 joined, and this watch then ran session
     * end and tore child1 down. But that fall may be a TRANSIENT of the MP
     * re-forming for a 2nd child, not the host truly leaving. With the gate set,
     * do not end the session on the fall - keep g_joiner_was_seated latched and
     * see whether the count recovers (MP re-stabilises for 3 stations) or stays
     * at 1 (a real loss). Off by default, so the verified 2-player graceful-leave
     * is unchanged. */
    static const bool no_watch = [] {
        const char* e = std::getenv("LSW2_NO_JOINER_WATCH");
        return e && *e == '1';
    }();
    if (no_watch) {
        static int said = 0;
        if (said < 8) {
            ++said;
            std::fprintf(stderr,
                         "[fork] joiner watch: count fell to %u but "
                         "LSW2_NO_JOINER_WATCH is set - NOT ending the session; "
                         "watching whether the count recovers\n",
                         players);
        }
        return;   /* leave g_joiner_was_seated latched; do not tear down */
    }

    /* TOLERATE A TRANSIENT FALL. The host bounces through a -1 reset at
     * char-select entry, which momentarily drops the count 2->1; ending the session
     * on that first frame is what made the joiner tear down + GX-wedge. Wait for the
     * count to STAY below 2 for kJoinerLossFrames consecutive frames before deciding
     * the host is really gone; a bounce recovers to >=2 and resets the streak above. */
    if (++g_joiner_low_streak < kJoinerLossFrames) {
        if (g_joiner_low_streak == 1)
            std::fprintf(stderr,
                         "[fork] joiner watch: count fell to %u - tolerating up to %d "
                         "frames in case the host is just bouncing\n",
                         players, kJoinerLossFrames);
        return;   /* keep the session; the host may recover (a transient bounce) */
    }

    g_joiner_was_seated = false;
    g_joiner_low_streak = 0;
    std::fprintf(stderr,
                 "[fork] joiner watch: the player count stayed at %u for %d frames "
                 "after a peer was seated - the host has really gone. Running the "
                 "cartridge's own session end so this console leaves cleanly instead "
                 "of raising a communication error\n",
                 players, kJoinerLossFrames);
    lsw2_wh_request_session_end("joiner left behind");

}

void lsw2_wh_local_index_experiment() {
    /* AN EXPERIMENT, off unless LSW2_WH_LOCAL_INDEX=1, and NOT a fix.
     *
     * WHAT IT IS TESTING. The active player index at netcount+0x2A is now the
     * ONLY measured difference between the two consoles: 0 on the host, 1 on
     * the child. Everything else about actor handling is byte-identical -
     * measured, both build an actor into slot 1 three times during level loads
     * and clear it three times, ending with slot 1 empty. So the
     * child's index points at a slot that is empty on BOTH consoles; on the
     * host that is harmless because index 0 holds the live actor, and on the
     * child it is fatal because FUN_0200E4EC's identity check can never match.
     *
     * WHY THIS IS WORTH MEASURING NOW AND WAS NOT BEFORE. An earlier analysis
     * rejected touching the index on the grounds that it is a correct station count and
     * the child's actor simply belongs at slot 1. The second half of that has
     * since been contradicted: nothing puts a lasting actor in slot 1 on either
     * console. With that gone, "is the index the whole difference?" is an open
     * question with a one-line answer.
     *
     * WHAT A RESULT WOULD MEAN. If forcing 0 makes the camera table appear and
     * the match start, then the index is being asked of the wrong thing - a
     * LOCAL actor array indexed by a GLOBAL station number - and the real fix
     * is to find where that conflation happens. If it changes nothing, the
     * index is not the whole story and this comes out.
     *
     * EITHER WAY THIS IS NOT THE FIX. Overwriting a value the cartridge
     * computed is a probe, not a repair, and it is removed once it has
     * answered. */
    static const bool on = [] {
        const char* e = std::getenv("LSW2_WH_LOCAL_INDEX");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build() || lsw2_wh_role_is_host()) return;

    /* EVERY FRAME, NOT ONCE. The session-established handler can run again, and
     * a one-shot write would be silently undone. Only written when it is wrong,
     * so a console that already agrees is untouched. */
    const uint32_t w = read32(kLsw2NetCountObject + 0x28u);
    if (((w >> 16) & 0xFFu) == 0u) return;

    write32(kLsw2NetCountObject + 0x28u, w & ~0x00FF0000u);

    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr,
                     "[fork] EXPERIMENT (whsession.h): forcing the child's "
                     "active player index from %u to 0, to test whether it is "
                     "the whole difference. MEASUREMENT, not a fix.\n",
                     unsigned((w >> 16) & 0xFFu));
    }
}

void lsw2_wh_nodetable_trace() {
    /* NAME THE FUNCTION THAT BUILDS THE NODE TABLE.
     *
     * WHAT THIS IS FOR, in plain English. Two consoles reach the cantina lobby
     * in one wireless session and the match will not start; the cartridge says
     * "Communication Error". That was traced to the child reloading level 9
     * without the netgame node table at [0x02109C88 + 0x21C], while the host's
     * reload builds it. Four candidate causes have been ruled out by
     * measurement, so guessing a fifth is not the move. The host DOES
     * build it, so the code that does exists and runs - this catches it in the
     * act and prints its address.
     *
     * THE WINDOW IS DELIBERATELY TINY. Deep trace records every branch on BOTH
     * cores and, left on, slows the run enough to stop it - that trap has cost
     * this project a build before. So the ring is armed only once the level has
     * come BACK (level 9 again) with the table still null, which the timeline
     * shows is a handful of frames before the build, and it is switched off the
     * moment the table appears or after kNodeTraceFrames either way.
     *
     * OFF UNLESS LSW2_TRACE_NODETABLE=1, because an instrument that can stop the
     * machine must never be on by accident. */
    static const bool on = [] {
        const char* e = std::getenv("LSW2_TRACE_NODETABLE");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build()) return;

    static bool done = false;
    if (done) return;

    const uint32_t table = read32(kLsw2NearestNodeOwner + 0x21Cu);
    const int level = lsw2_cheats_current_level();

    /* Phase 1: wait for the unload, so this cannot arm on the boot load. */
    /* WHICH UNLOAD TO WATCH. The first is the BOOT load, and the first attempt
     * armed on it and learned nothing new - the child builds a table perfectly
     * well at boot. The interesting one is the SECOND: the reload that follows
     * the join, which is the one that builds nothing. Comparing the two on the
     * same console is the whole point, so the unloads are counted rather than
     * taken as they come. */
    static const int want_unload = [] {
        const char* e = std::getenv("LSW2_TRACE_NODETABLE_NTH");
        const long n = e ? std::strtol(e, nullptr, 0) : 0;
        return (n > 0 && n < 10) ? int(n) : 2;
    }();

    static int unloads = 0;
    static bool in_unload = false;
    if (level == 0 && table == 0) {
        if (!in_unload) {
            in_unload = true;
            ++unloads;
            std::fprintf(stderr,
                         "[fork] node trace: level unload #%d (watching for "
                         "#%d)\n", unloads, want_unload);
        }
    } else if (level > 0) {
        in_unload = false;
    }
    if (unloads < want_unload) return;

    /* Phase 2: the level is back and the table is not - arm here. */
    static bool armed = false;
    static int frames = 0;
    if (!armed) {
        if (level <= 0 || table != 0) return;
        armed = true;
        frames = 0;
        runtime_set_deep_trace(1);
        runtime_trace_reset();
        std::fprintf(stderr,
                     "[fork] node trace: armed at level %d with the table still "
                     "null - the build is due within a few frames\n", level);
        return;
    }

    ++frames;

    const bool built = (table != 0);
    if (!built && frames < kNodeTraceFrames) return;

    done = true;
    runtime_set_deep_trace(0);      /* OFF IMMEDIATELY, always. */

    if (!built) {
        std::fprintf(stderr,
                     "[fork] node trace: %d frames and the table is STILL null - "
                     "deep trace off. On this console nothing built it.\n",
                     kNodeTraceFrames);
        return;
    }

    static RuntimeTraceEntry ring[kNodeTraceEntries];
    const uint32_t n = runtime_trace_copy_recent(ring, kNodeTraceEntries);

    uint32_t arm9 = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (ring[i].pc >= kNodeCodeLow && ring[i].pc < 0x02400000u) ++arm9;
    const uint32_t skip = arm9 > kNodeShowLast ? arm9 - kNodeShowLast : 0u;

    std::fprintf(stderr,
                 "[fork] node trace: the table appeared at %08X after %d "
                 "frame(s) - last %u of %u ARM9 branches before it:\n",
                 unsigned(table), frames,
                 unsigned(arm9 < kNodeShowLast ? arm9 : kNodeShowLast),
                 unsigned(arm9));

    /* DEDUPE CONSECUTIVE REPEATS. The first dump was forty lines of the same
     * two addresses - a memset/memcpy loop at 0x020CF21C/0x020CF220 that fills
     * the ring and buries every real call site. A tight loop is one fact, not
     * forty, so it is printed once with its count. */
    uint32_t seen = 0, run = 0, last_pc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        const RuntimeTraceEntry& e = ring[i];
        if (e.pc < kNodeCodeLow || e.pc >= 0x02400000u) continue;
        if (seen++ < skip) continue;
        if (e.pc == last_pc) { ++run; continue; }
        if (run) std::fprintf(stderr, "      (repeated %u more)\n", unsigned(run));
        run = 0;
        last_pc = e.pc;
        std::fprintf(stderr, "   pc=%08X lr=%08X r0=%08X r4=%08X\n",
                     unsigned(e.pc), unsigned(e.r14), unsigned(e.r0),
                     unsigned(e.r4));
    }
    if (run) std::fprintf(stderr, "      (repeated %u more)\n", unsigned(run));
}

void lsw2_wh_join_gap_report() {
    /* WHAT DOES THE CHILD LACK THAT THE HOST HAS?
     *
     * PLAIN ENGLISH, AND WHICH GOAL THIS SERVES. Two consoles now meet in the
     * cantina lobby with a live session, and the match will not
     * start: the cartridge says "Communication Error". The child
     * faults and the host does not, so the two differ in something, and every
     * guess about WHAT would be cheaper to check than to argue.
     *
     * WHERE THE CHILD BREAKS, traced back to its first fault rather than its
     * loudest one. The first bad read is `pc=020A97E0 lr=020A984C`, inside
     * FUN_020A9804, which is a NEAREST-NODE SEARCH:
     *
     *     count = FUN_02067B54([this + 0x21C]);        // a byte at table+4
     *     for (i = 0; i < count; ++i) {
     *         node = FUN_02067B48([this + 0x21C], i);  // *table + i*0x10
     *         d    = FUN_020A97E0(this, node, pos);    // squared distance
     *     }
     *
     * The count came back NON-ZERO and the data pointer at table+0 is null, so
     * it walks a table that says it has entries and does not. Neither helper
     * has any guard - FUN_02067B48 is literally `return *p + i*0x10`.
     *
     * SO THIS PRINTS THE THREE THINGS THAT COULD DIFFER, on both consoles, and
     * changes nothing. The wireless owner's two bytes are here because they are
     * exactly what the join role skips: the host runs the whole of 0x020B10A8,
     * whose tail is `owner[0xC] = 5; owner[6] = 1`, while the child stops after
     * the view-manager allocator. If those differ, that is the lead; if they do
     * not, the lead is dead and no write was made on the strength of it. */
    if (!ui_fork_build()) return;

    /* A TIMELINE, NOT ONE SNAPSHOT. The first version printed once, when both
     * consoles were already in the lobby, and answered "they differ" without
     * answering WHEN they started to. The table appears on the host at some
     * moment and never on the child, and the moment is the whole question - so
     * this prints whenever the state id or the table pointer changes, on both
     * consoles, and stays quiet otherwise. */
    const uint32_t table_now = read32(kLsw2NearestNodeOwner + 0x21Cu);
    const int state_now = lsw2_state_current();
    const int level_now = lsw2_cheats_current_level();
    static uint32_t last_table = 0xFFFFFFFFu;
    static int last_state = -999;
    static int last_level = -999;
    if (table_now == last_table && state_now == last_state &&
        level_now == last_level)
        return;
    last_table = table_now;
    last_state = state_now;
    last_level = level_now;

    const uint32_t owner = read32(kLsw2WirelessOwner + kLsw2WirelessOwnerField);
    /* FOUR BYTES, NOT ONE. FUN_020B111C is the cartridge's session teardown and
     * it clears +6, +7, +8 and +0xD together, and GUARDS itself on +8 - so the
     * role bookkeeping is spread across them, and reading only +6 could not say
     * whether the child has client-side state of its own. */
    uint32_t b6 = 0xFFu, b7 = 0xFFu, b8 = 0xFFu, bd = 0xFFu;
    if (owner >= 0x02000000u && owner < 0x02400000u) {
        const uint32_t w4 = read32(owner + 4u);
        const uint32_t wc = read32(owner + 0xCu);
        b6 = (w4 >> 16) & 0xFFu;
        b7 = (w4 >> 24) & 0xFFu;
        b8 = read32(owner + 8u) & 0xFFu;
        bd = (wc >> 8) & 0xFFu;
    }

    /* The nearest-node search's own object and table, read the way it reads
     * them - pointer at +0, count byte at +4. */
    /* WHAT DECIDES WHETHER A CAMERA COMMAND CAN FIRE AT ALL.
     *
     * The builder is FUN_0200E738, command type 2, installing the
     * camera-node table. Its caller FUN_0200E4EC is a per-frame trigger sweep
     * that opens with an early return:
     *
     *     if (param_1 != *(int *)(0x02109358 + (u8)[0x02102822] * 4)) return;
     *
     * - "am I the ACTIVE player's actor?" - and then walks the level's list of
     * camera trigger volumes at *(0x0210930C + 4), testing each against the
     * actor's position. So a command fires only if there is an active player
     * index, an actor at it, and a trigger list to walk. Any of the three being
     * wrong on the child would explain the missing table, and each is one read.
     *
     * The two bases already have names - the index byte lives in netcount.h's
     * object and the list in levels.h's level root - so they are reused rather
     * than redeclared. */
    const uint32_t player_ix = (read32(kLsw2NetCountObject + 0x28u) >> 16) & 0xFFu;
    const uint32_t active_actor = read32(kLsw2CameraActorArray + player_ix * 4u);
    const uint32_t trigger_list = read32(kLsw2LevelRootAddr + 4u);

    /* ALL FOUR SLOTS, because "the actor at the active index is null" and "the
     * actor array is empty" are different faults with different fixes, and one
     * read each tells them apart. */
    uint32_t slots[4];
    for (int i = 0; i < 4; ++i)
        slots[i] = read32(kLsw2CameraActorArray + uint32_t(i) * 4u);

    const uint32_t table = table_now;
    uint32_t data = 0, count = 0;
    if (table >= 0x02000000u && table < 0x02400000u) {
        data = read32(table);
        count = read32(table + 4u) & 0xFFu;
    }

    std::fprintf(stderr,
                 "[fork] JOIN GAP (%s) state %d level %d: owner+6=%u +7=%u "
                 "+8=%u | vm=%08X | camera: player=%u actor=%08X triggers=%08X "
                 "[%08X %08X %08X %08X] | node table=%08X data=%08X count=%u%s\n",
                 lsw2_wh_role_is_host() ? "HOST" : "JOIN", state_now,
                 lsw2_cheats_current_level(),
                 unsigned(b6), unsigned(b7), unsigned(b8),
                 unsigned(read32(kLsw2ViewManagerPtr)),
                 unsigned(player_ix), unsigned(active_actor),
                 unsigned(trigger_list),
                 unsigned(slots[0]), unsigned(slots[1]),
                 unsigned(slots[2]), unsigned(slots[3]),
                 unsigned(table), unsigned(data), unsigned(count),
                 (count && !data) ? "   <- COUNT WITHOUT DATA" : "");
}

void lsw2_wh_reopen_chooser_when_joined() {
    /* THE HOST REOPENS GAME SETTINGS ONCE A GUEST IS ACTUALLY THERE.
     *
     * WHAT THIS IS FOR. Both consoles now get into the cantina lobby with
     * a live session - `players 2`, the netgame pump running, messages flowing
     * both ways. The match still has to be started, and the screen that starts
     * it is state 54, the map/gamemode chooser. The probe that first asked for
     * it is one-shot, so after the rendezvous nothing could ask again.
     *
     * WHY THIS IS THE HOST'S JOB AND NOT THE GUEST'S. In the cartridge's own
     * flow the player hosting the session is the one who picks the map, the
     * mode and the time limit and presses Start Game; a joiner waits. So this
     * is gated on the host role, and on a guest genuinely being present rather
     * than on a timer - `players` comes from the cartridge's own netgame
     * bookkeeping, not from anything the fork counts for itself. */
    if (!ui_fork_build() || !lsw2_wh_role_is_host()) return;
    if (!g_probe_asked) return;              /* the first ask has not happened */

    static bool done = false;
    if (done) return;

    if (lsw2_viewmanager_state().players < 2) return;
    if (lsw2_state_current() != kLsw2ArenaLobbyState) return;
    if (!lsw2_guest_ready()) return;

    done = true;
    std::fprintf(stderr,
                 "[fork] a guest has joined (players %d) - reopening the "
                 "cartridge's Game Settings so the host can start the match\n",
                 lsw2_viewmanager_state().players);

    lsw2_guest_call(lsw2_arm9_FUN_0205ecc4, kLsw2StateRequester,
                    kLsw2StateManager, 0x36u, 1u, 1u, 2, 0u, 0u);
}

/* THE JOINER FOLLOWS THE HOST INTO THE MATCH. Armed by the fork tick
 * when the host's start-match signal arrives over the transport; fires once the
 * guest is awake and the joiner is settled in the arena lobby (state 48).
 *
 * It requests the two arena states FUN_02072cf4 asks for on a joining player -
 * state 40 (room reload) then 41 (arena load). It uses the cartridge's
 * own requester, which only ENQUEUES the request: the heavy kind-4 level rebuild
 * runs later in the guest's OWN frame when the state machine enters the state, so
 * this sidesteps the guest-call GX-IRQ wall that a direct FUN_02072cf4 call hits.
 * The earlier crash in the arena flow (its tail deref of a null view
 * manager) no longer applies: the JOIN-role hop builds the joiner's
 * view manager before this ever fires.
 *
 * Behind LSW2_JOINER_ENTER_MATCH while it is proven; off by default so the
 * deployed build is unchanged. Returns true once it has fired. */
namespace { bool g_joiner_enter_armed = false; }

void lsw2_wh_arm_joiner_enter_match() { g_joiner_enter_armed = true; }

void lsw2_wh_joiner_enter_match() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_JOINER_ENTER_MATCH");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build() || lsw2_wh_role_is_host()) return;
    if (!g_joiner_enter_armed) return;

    static bool done = false;
    if (done) return;

    if (lsw2_state_current() != kLsw2ArenaLobbyState) return;   /* settle first */
    if (!lsw2_guest_ready()) return;                            /* not while halted */

    done = true;
    std::fprintf(stderr,
                 "[fork] joiner enter-match: host signalled; requesting arena "
                 "states 0x28 then 0x29 via the cartridge's own requester (view "
                 "manager %08X) - the rebuild runs in the guest's own frame\n",
                 unsigned(read32(kLsw2ViewManagerPtr)));

    lsw2_guest_call(lsw2_arm9_FUN_0205ecc4, kLsw2StateRequester,
                    kLsw2StateManager, 0x28u, 1u, 1u, 2, 0u, 0u);
    lsw2_guest_call(lsw2_arm9_FUN_0205ecc4, kLsw2StateRequester,
                    kLsw2StateManager, 0x29u, 1u, 1u, 2, 0u, 0u);
}

/* FORCE 41 -> 42, the bounded probe. The real trigger is receiving
 * netgame message 0x0D, whose dispatcher case '\r' requests state 0x2a (42). The
 * fork does not relay the netgame message stream, so neither console gets that
 * 0x0D and both stall at 41. This asks for state 42 directly, the way case '\r'
 * does, once the console has settled at 41 - to learn whether forcing the state
 * yields a playable arena (a lead) or a desync/comm-error (proof that 42 needs
 * the real handshake, i.e. the message relay is unavoidable). Behind
 * LSW2_FORCE_MATCH; off by default. The settle delay lets both consoles reach 41
 * before either jumps, since they entered it together. */
void lsw2_wh_force_match_state() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_FORCE_MATCH");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build()) return;

    static bool done = false;
    if (done) return;

    /* SETTLE. Count every frame at 41 (not only guest-ready ones), reset when we
     * leave it, so both consoles are there before the jump. */
    static int settle = 0;
    if (lsw2_state_current() != 41) { settle = 0; return; }
    if (++settle < 150) return;   /* ~6 s at ~24 fps */

    if (!lsw2_guest_ready()) {
        static int said = 0;
        if (++said % 120 == 1)
            std::fprintf(stderr,
                         "[fork] force match: settled at 41 but the guest is "
                         "not ready for a call (waiting)\n");
        return;
    }

    done = true;
    std::fprintf(stderr,
                 "[fork] force match: settled at state 41, guest ready; "
                 "requesting state 0x2a (42) as the msg-0x0D handler does "
                 "(probe)\n");

    lsw2_guest_call(lsw2_arm9_FUN_0205ecc4, kLsw2StateRequester,
                    kLsw2StateManager, 0x2au, 1u, 1u, 2, 0u, 0u);
}

void lsw2_wh_probe_chooser() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_PROBE_CHOOSER");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build() || g_probe_asked) return;

    /* Only from a settled arena lobby - a chooser asked for before the game
     * has a state object at all was measured producing its own damage, which
     * would confuse the trace this exists to take. */
    if (lsw2_state_current() < 0) return;
    if (lsw2_cheats_current_level() != kLsw2ArenaLobbyLevel) return;
    if (++g_probe_wait < kProbeAfterFrames) return;
    if (!lsw2_guest_ready()) return;

    /* THE PREREQUISITE, WHICH THE REAL GAME ALWAYS DOES FIRST.
     *
     * A player hosting a wireless game presses "Host Wireless" - 0x020B10A8 -
     * and only then reaches this chooser. That function's first act builds the
     * view manager, and state 54 makes a virtual call through it.
     * Asking for the chooser without it is the whole of the crash, so ask for
     * the manager first and do not request the state until there is one.
     *
     * NOT MARKED ASKED ON FAILURE. `kLsw2ViewManagerNotNow` means the guest was
     * halted this frame, which is ordinary; the tick simply tries again. The
     * two real failures are reported by the callee and are worth retrying too,
     * since the guard they hit may only be false early in a level. */
    const int vm = lsw2_wh_make_view_manager();
    if (vm < 0) return;

    /* g_probe_asked IS NO LONGER SET HERE. It used to latch on the first
     * request, which is what made "asked" and "arrived" the same thing in this
     * file - and they are not. It now latches below, when the machine actually
     * reports the state, so everything gated on it (the selection write, the
     * chooser reopen) waits for arrival rather than for an intention.
     *
     * AND THE TRACE ARM IS ONE-SHOT, which it stopped being for one build. This
     * line used to run once because the function returned after its single
     * request; with the retry below it began running EVERY TICK, re-arming the
     * branch ring continuously - and deep trace stops the run if it is left on.
     * The retry then looked broken when what had actually happened is that the
     * console was crawling. Turning a one-shot into a per-tick statement by
     * moving the code around it is an easy way to break something that has
     * nothing to do with the change. */
    static bool armed_once = false;
    if (!armed_once) {
        armed_once = true;
        g_probe_dump_in = kProbeArmAt;
    }

    /* THE RING IS ARMED LATE, NOT HERE.
     *
     * It holds 4096 entries and the ARM7's idle loop fills all of them in a
     * fraction of a frame, so arming it at the request captures the request
     * and nothing else - the interesting moment is thirty-five frames later,
     * when the walk happens and the frames stop. kProbeArmAt is chosen to land
     * just before that. */
    /* WHICH SCREEN, AND WHY IT DEPENDS ON THE ROLE.
     *
     * A NitroSDK WH session needs one PARENT and one CHILD: the parent beacons,
     * the child scans and connects. Measured: running the same host
     * path on both consoles makes two parents, both reach DATASHARING, and
     * `players` stays 1 on each - two solo sessions rather than one match.
     *
     * So the two consoles take different doors, and both doors are the
     * cartridge's own:
     *
     *   HOST goes to state 54, the map/gamemode chooser, having already run the
     *   whole of 0x020B10A8 in the hop.
     *
     *   JOIN goes to state 0x22, which coopjoin.cpp identifies as
     *   the cartridge's "Browsing Wireless Game" - the list screen whose connect
     *   is 0x020ADBCC, the child side of the session. The join role's hop
     *   deliberately runs ONLY the view-manager allocator and not the session
     *   start, because starting a session is exactly what a child must not do. */
    /* WHICH MENU THE HOST ASKS FOR, AND WHY IT IS NOW A CHOICE.
     *
     * THE CARTRIDGE'S OWN WIRELESS MENU IS STATE 53, NOT 54. The state factory
     * FUN_0205F574 is `cmp r9,#0x39; addls pc,pc,r9,lsl #2`, so case N's branch
     * sits at 0x0205F59C + N*4. Case 53 is `b 0x0205FE00`, whose arm allocates
     * 0xF4 bytes and calls `bl 0x0208E260` at 0x0205FE28 - the constructor that
     * installs vtable 0x020FF524, whose handler is FUN_0208EA58. That handler's
     * case 1 is the one that calls 0x020B10A8, the session start, and then asks
     * for state 15. Case 54 branches to 0x0205FC94 instead: a different class
     * (vtable 0x020FFB10), which is what was measured on the live object.
     *
     * So the screen the fork has been driving is not the one the cartridge uses
     * to host a wireless game, and the fork has been calling the session start
     * itself because of it. LSW2_WH_MENU lets the real route be tried without
     * disturbing the one that already gets two consoles connected. */
    const uint32_t want =
        lsw2_wh_role_is_host() ? host_menu_state() : uint32_t(kLsw2BrowseState);

    /* ASK AGAIN UNTIL IT LANDS, because asking once is not the same as getting
     * there.
     *
     * The request is written into the machine's slot and the machine consumes
     * it when it chooses - it was measured sitting PENDING for 34 frames -
     * and sometimes it is never consumed at all. That is the whole reason the
     * two-console runs looked flaky: whenever the host's state-54 request went
     * unconsumed the host stayed in the lobby, was not discoverable, and the
     * child's browse list came up empty. It was never the taps.
     *
     * So the request repeats on a slow cadence until the state actually
     * reports the state that was asked for. Bounded, because a request that
     * never lands after this many tries is a fact worth printing rather than a
     * loop worth running forever. */
    static int asked = 0;
    static int wait_for = 0;

    if (lsw2_state_current() == int(want)) {
        if (asked) {
            std::fprintf(stderr,
                         "[fork] PROBE: state %u REACHED after %d request(s)\n",
                         unsigned(want), asked);
            asked = 0;
            g_probe_asked = true;      /* latch: stop asking, start reporting */
        }
        return;
    }

    if (wait_for > 0) { --wait_for; return; }
    if (asked >= kProbeMaxAsks) {
        static bool gave_up = false;
        if (!gave_up) {
            gave_up = true;
            std::fprintf(stderr,
                         "[fork] PROBE: asked for state %u %d times and the "
                         "machine never entered it - the run's precondition has "
                         "NOT been met, so nothing measured after this line "
                         "means what it appears to.\n",
                         unsigned(want), kProbeMaxAsks);
        }
        return;
    }

    ++asked;
    wait_for = kProbeAskGap;
    std::fprintf(stderr,
                 "[fork] PROBE: role=%s - asking for state %u from state %d "
                 "(attempt %d)\n",
                 lsw2_wh_role_is_host() ? "HOST" : "JOIN", unsigned(want),
                 lsw2_state_current(), asked);

    /* Six arguments, as recorded from the arena flow's own call:
     * FUN_0205ecc4(manager, 0x36, 1, selector, 0, 0). */
    lsw2_guest_call(lsw2_arm9_FUN_0205ecc4, kLsw2StateRequester,
                    kLsw2StateManager, want, 1u, 1u, 2, 0u, 0u);
}

void lsw2_wh_probe_dump_trace() {
    /* AN UNCONDITIONAL HEARTBEAT, BEFORE EVERY OTHER TEST.
     *
     * "The frame loop stalls" was inferred three times from an ABSENCE - the
     * repeated slot dumps stopped printing and the log went quiet - and an
     * absence cannot tell "the tick stopped" from "the tick runs and every path
     * through it returns early". This is the same mistake the WH drain and the
     * chooser gate each cost a build to find, so it gets the same fix: count,
     * and say the count. */
    if (g_probe_asked) {
        static long long beats = 0;
        /* EVERY FIFTH BEAT, NOT EVERY SIXTIETH. "Printed once" only bounded
         * the tick at 1..59 frames, which is not an answer - the whole point
         * is the exact frame the tick stops on. */
        if (++beats <= 200 && (beats == 1 || (beats % 5) == 0)) {
            std::fprintf(stderr,
                         "[fork] PROBE heartbeat: the fork tick has run %lld "
                         "time(s) since the request\n", beats);
        }
    }

    if (g_probe_dump_in < 0) return;
    if (--g_probe_dump_in > 0) return;

    /* TWO PHASES. Pass one ARMS the ring, a few frames before the walk is due;
     * pass two READS it. Doing both at once was the first version and it
     * captured the request rather than the failure. */
    static int phase = 0;
    ++phase;

    if (phase == 1) {
        runtime_set_deep_trace(1);
        runtime_trace_reset();
        g_probe_dump_in = kProbeReadAfter;
        std::fprintf(stderr,
                     "[fork] PROBE: ring armed, reading it in %d frames" "\n",
                     kProbeReadAfter);
        return;
    }

    g_probe_dump_in = -1;

    /* OFF AGAIN IMMEDIATELY. Recording every branch on both cores costs enough
     * that the run stops making progress, and an instrument that stops the
     * machine is worse than none. */
    runtime_set_deep_trace(0);

    /* THE MACHINE'S TWO SLOTS. FUN_0205F168 is what calls a state's update -
     * vtable +0x0C - and it has exactly two guards: a pending request in the
     * slot, and bit 1 of [state + 0x20]. Both are four loads away. The machine
     * is at 0x02102900 ITSELF, not a pointer to it. */
    for (int slot = 0; slot < 2; ++slot) {
        const uint32_t obj = read32(kLsw2StateManager + uint32_t(slot) * 4u);
        const uint32_t req =
            read32(kLsw2StateManager + uint32_t(slot) * 0x20u + 0x48u);
        uint32_t id = 0xFFFFFFFFu, flags = 0, vt = 0;
        if (obj >= 0x02000000u && obj < 0x02400000u) {
            id = read32(obj + 4u) & 0xFFu;
            vt = read32(obj);
            flags = read32(obj + 0x20u);
        }
        std::fprintf(stderr,
                     "[fork] PROBE slot %d: obj=%08X id=%d vtable=%08X "
                     "request=%08X%s flags=%08X%s" "\n",
                     slot, unsigned(obj), int(id), unsigned(vt), unsigned(req),
                     req ? " (PENDING)" : "", unsigned(flags),
                     (flags & 2u) ? " (bit1 set)" : "");
    }

    static RuntimeTraceEntry ring[kProbeTraceEntries];
    const uint32_t n = runtime_trace_copy_recent(ring, kProbeTraceEntries);

    /* ARM9 O"\n"Y - main RAM and ITCM. The ARM7's BIOS is below 0x01FF8000 and
     * its WRAM above 0x02400000, and its idle loop would drown the useful
     * entries. Counted first so the LAST forty ARM9 events are printed: the
     * failure is at the end. */
    uint32_t arm9 = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (ring[i].pc >= 0x01FF8000u && ring[i].pc < 0x02400000u) ++arm9;
    const uint32_t skip = arm9 > kProbeShowLast ? arm9 - kProbeShowLast : 0u;

    std::fprintf(stderr, "[fork] PROBE trace: %u event(s), %u ARM9, last %u" "\n",
                 unsigned(n), unsigned(arm9),
                 unsigned(arm9 < kProbeShowLast ? arm9 : kProbeShowLast));
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const RuntimeTraceEntry& e = ring[i];
        if (e.pc < 0x01FF8000u || e.pc >= 0x02400000u) continue;
        if (seen++ < skip) continue;
        std::fprintf(stderr,
                     "   pc=%08X addr=%08X kind=%u lr=%08X r0=%08X r4=%08X" "\n",
                     unsigned(e.pc), unsigned(e.addr), unsigned(e.kind),
                     unsigned(e.r14), unsigned(e.r0), unsigned(e.r4));
    }
}

void lsw2_wh_probe_update() {
    /* `this` IN r0 - the ARM C++ ABI's implicit first argument.
     *
     * ONCE, ON THE FIRST UPDATE. After that the state's own logic owns the
     * field and writing it every frame would fight whatever it does next. */
    const uint32_t obj = g_cpu.R[0];
    if (!g_probe_selected && obj >= 0x02000000u && obj < 0x02400000u) {
        g_probe_selected = true;
        const uint32_t was = read32(obj + 0xECu);
        const uint32_t target = read32(obj + 0xF0u);
        write32(obj + 0xECu, 1u);
        std::fprintf(stderr,
                     "[fork] PROBE: state 54's +0x08 reached - "
                     "selection %u -> 1, default target state %u" "\n",
                     unsigned(was), unsigned(target));
    }

    /* The game's own update, by symbol so it cannot recurse into this. */
    g_cpu.R[15] = kLsw2ChooserEnter;
    lsw2_arm9_FUN_0208e374();
}
