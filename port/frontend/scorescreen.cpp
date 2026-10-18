/* See scorescreen.h. */

#include "scorescreen.h"
#include "cheats.h"
#include "levels.h"
#include "lobbyhud.h"
#include "netcount.h"
#include "netmsg.h"      /* kLsw2MsgQuit */
#include "statelog.h"
#include "statereq.h"
#include "vwh.h"
#include "whsession.h"

#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstdlib>

namespace {

/* The manager, the state it points at, and the id the dispatcher compares -
 * all reached from one static. */
/* See statelog.h - kLsw2StateManager is the one copy. */
/* The state manager, its two +4 hops and the state ids are ALL statelog.h's
 * now - kLsw2StateManager, kLsw2StateCurrent, kLsw2StateId and the ids. This
 * file used to declare the offsets and the ids again under its own names,
 * directly beneath a comment saying the manager was the one copy. */
/* CONFIRMED FROM A RECORDING, 2026-08-30: state 0x2C (44) is what the game
 * enters at the end of each of the three matches in gpudump_3matches, on
 * exactly the frames the arena HUD's slot column vanishes. See the note in
 * statelog.h. This id had been read out of the code and never watched. */

/* The selected row, read three ways in the code: the enter writes 0 here, the
 * draw loop compares each row against it, and 0x0209CB94 dispatches on it. */
constexpr uint32_t kScoreSelection = 0x8Cu;
constexpr uint8_t kRowContinue = 0;

/* The game's own seconds counter - the statue select's 30s timer reads the same
 * word. */
/* See lobbyhud.h - kLsw2SecondsClock is the one copy. */

constexpr uint16_t kKeyA = 1u << 0;

/* HOW LONG THE PRESS IS HELD.
 *
 * THIS WAS 1, AND ONE FRAME WAS NOT ENOUGH - MEASURED. The old
 * comment reasoned: "the guest samples input once a frame and the menu acts on
 * a newly-pressed bit, so one frame is enough and two would risk the next
 * screen seeing an A it did not ask for."
 *
 * The first half is an assumption about a 1:1 relationship between a port frame
 * and a guest sample, and this console does not run at 1:1 - it runs at roughly
 * 0.44x real time (measured). A single-port-frame press is exactly the shape
 * this project has twice recorded being dropped: tools/pilot.py grew its
 * `--frames` argument because one-frame taps vanish, and two diagnoses were
 * wrong before that was understood.
 *
 * THE SECOND HALF WAS A REAL RISK WITH A BETTER ANSWER THAN A SHORT PRESS.
 * Rather than hoping the press is over before the next screen arrives, the key
 * is released the moment the score screen is no longer up - see
 * lsw2_score_auto_keys(). A held press cannot leak into the next screen if it
 * stops existing when this one does, so the hold can be generous. */
constexpr int kPressFrames = 8;

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;
bool ram_pointer(uint32_t v) { return v >= kRamLow && v < kRamHigh; }

uint32_t g_state = 0;        /* the score state, while one is up */
uint32_t g_entered = 0;      /* the clock when it came up        */
bool g_armed = false;        /* we have seen this visit start    */
bool g_fired = false;        /* and already pressed for it       */
int g_press_left = 0;
int g_watch_left = 0;        /* frames still to report after a press      */
bool g_screen_up = false;    /* refreshed each tick, read by auto_keys    */
bool g_want_lobby = false;   /* the return is owed and not yet accepted   */
bool g_state_asked = false;  /* the state half is in; the level half is not */

/* The input manager FUN_0205ce64 publishes the pad into: four 10-byte per-slot
 * records, buttons in the first halfword, the local player's index at +0x2A.
 * Read-only here, and only while a press is being watched.
 *
 * THE ADDRESS AND THE LOCAL INDEX ARE netcount.h's, not new ones. The
 * dup_constants gate refused a private copy and it was right to: the same
 * object is already named kLsw2NetCountObject there, with the local station at
 * kLsw2NetLocalStation. Two names for one address is how a structure gets
 * described two different ways and one of them turns out to be wrong. Only the
 * stride is added here, because netcount does not walk the per-slot records. */
constexpr uint32_t kInputStride = 10u;

void write_guest_byte(uint32_t addr, uint8_t v) {
    const uint32_t base = addr & ~3u;
    const uint32_t shift = (addr & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFu << shift)) | (uint32_t(v) << shift));
}

uint32_t read_guest_bytes(uint32_t addr, uint32_t width) {
    const uint32_t shift = (addr & 3u) * 8u;
    const uint32_t mask = width >= 4u ? 0xFFFFFFFFu : ((1u << (width * 8u)) - 1u);
    return (bus_device_read32(0, addr & ~3u) >> shift) & mask;
}

/* THE HEARTS THE LOBBY WILL RESTORE - the top-screen hearts bug.
 *
 * The return asks for the lobby in CoopEnter, and CoopEnter's spawn
 * (FUN_02089c88 -> FUN_02089e60 -> FUN_0208a31c) restores every gated party
 * record into the new actor: its character, and its hearts from the record's
 * hearts byte. The cartridge never enters a level that way without first
 * snapshotting the party (FUN_02088b84 -> FUN_02089388). FUN_020b111c does it
 * right before its own CoopEnter request, and state 40's entry FUN_020733b4
 * does it before the join's reload. This return has no snapshot. The records
 * it leaves behind were last written by state 41, which rebuilds the record
 * array (FUN_0208896c: a 0xFF fill that never sets the hearts byte) and gates
 * each seated player's record (0x02098C4C). So the lobby restored 255 hearts.
 * The HUD's show step (0x0208AAF0) had just reset its cache to 0xFF, and its
 * draw (0x0208AFB0) redraws only when the count differs from the cache. 255
 * did not, so the map stayed the blank tile 0x40 that the show step's clear
 * left. Measured on two consoles (conn_hearts2/3): write watches on the first
 * heart cell 0x060000CC and on the record's hearts word; both actors at 255 of
 * 4 after the return.
 *
 * NOT THE WHOLE SNAPSHOT. It would also store the arena's positions, and
 * CoopEnter moves the new actors to wherever the records say (FUN_02089c88).
 * For the cartridge that is a re-entry of the same level; here it is a
 * different level. So this writes only the byte the lobby restores, and with
 * the count the game builds every player with: FUN_0200d6f4, the player
 * actor's constructor, sets both the count (+0x6B6) and the most (+0x6B8) to
 * 4, and FUN_02089ba8's party reset writes the same 4. A fresh lobby visit
 * starts where a fresh player does.
 *
 * NOT THE ARENA ACTORS' OWN COUNT. The first version read each seated slot's
 * actor and copied its maximum; at the dataset that carries the return it
 * found no live actor for any gated record and wrote nothing (conn_hearts4),
 * and the hearts stayed blank.
 *
 * Written at the dataset boundary like the two requests, so every console
 * writes the same bytes at the same step. The count is cheats.h's
 * kLsw2PlayerBuiltHearts. */
void refill_party_hearts() {
    for (uint32_t i = 0; i < kLsw2PartySlots; ++i) {
        const uint32_t rec = kLsw2AppearanceRecord + i * kLsw2AppearanceStride;
        if (read_guest_bytes(rec + kLsw2AppearanceGate, 1u) == 0u) continue;
        const uint32_t had = read_guest_bytes(rec + kLsw2AppearanceHearts, 1u);
        write_guest_byte(rec + kLsw2AppearanceHearts, kLsw2PlayerBuiltHearts);
        std::fprintf(stderr, "[fork] lobby return: player %u's record restores "
                     "%u hearts (it held %u)\n",
                     unsigned(i), unsigned(kLsw2PlayerBuiltHearts), unsigned(had));
    }
}

/* ONE STEP OF THE OWED RETURN - the state, then the level (see the long note in
 * lsw2_score_tick for why in that order). Called from the tick until it clears
 * g_want_lobby. Alone only: a shared session returns through
 * lsw2_score_lobby_return_begin instead. */
void serve_lobby_return() {
    if (!g_state_asked) {
        if (lsw2_wh_write_state_request(kLsw2StateDefaultArm)) {
            g_state_asked = true;
            std::fprintf(stderr,
                         "[fork] score screen: asked for state %d, which "
                         "dismisses the score screen; the level request "
                         "follows and must land LAST\n",
                         kLsw2StateDefaultArm);
        }
    } else if (lsw2_cheats_warp_cold(kLsw2ArenaLobbyLevel)) {
        g_want_lobby = false;
        g_state_asked = false;
        std::fprintf(stderr,
                     "[fork] score screen: asked for level %d - THE "
                     "FORK'S RETURN, which no cartridge row performs\n",
                     kLsw2ArenaLobbyLevel);
    }
}

}  // namespace

void lsw2_score_lobby_return_begin() {
    /* THE GAME'S OWN PAIR, IN ONE STEP, THE GAME'S OWN WAY.
     *
     * The session lobby reload every join runs - state 40's own code - asks
     *
     *     0x02073964  0x0205ECC4(m, 4,    world,  0x0E CoopEnter, level 9)
     *     0x02073980  0x0205ECC4(m, 0x30, screen, 0)
     *
     * together, and the manager fades each slot out, exits and destroys what
     * it held, then builds - the level before the screen, which reads the
     * level's party. The alone path below (serve_lobby_return) instead asks for
     * 48 a step before the level, in whichever slot is current, and builds the
     * level by setting the status byte itself - skipping the old world's exit
     * and destructor. After a four-player match that left the arena's
     * registrations live under the new lobby: 48 came up over no level, and all
     * four consoles' sprite loaders read sources that are not data (0x01000198,
     * 0x03000000), then the geometry engine wedged.
     *
     * Written here, at the dataset boundary, on every console - so the pair
     * lands at the same step everywhere, as the cartridge's would. */
    std::fprintf(stderr, "[fork] score screen: the host's lobby return arrived "
                 "with this dataset - every console asks for the session "
                 "lobby at this step\n");
    g_want_lobby = false;
    g_state_asked = false;
    lsw2_lobby_return_pair();
}

bool lsw2_score_quit_message(uint32_t packet) {
    if (!ui_fork_build() || !lsw2_vwh_lockstep()) return false;
    const uint32_t type = read_guest_bytes(packet + 1u, 1u);
    if (type != kLsw2MsgQuit) return false;
    std::fprintf(stderr, "[fork] score screen: Return to Lobby chosen - the "
                 "session stays up and every console returns to the lobby at "
                 "this step (the cartridge's Quit would end the session)\n");
    lsw2_score_lobby_return_begin();
    return true;
}

void lsw2_lobby_return_pair() {
    /* THE HEARTS WITH THE PAIR, for every caller. The return from the
     * communication-error screen (fork.cpp) asked for the pair without them,
     * and the client came home with no hearts on its HUD - the same 255 the
     * note above refill_party_hearts measured. */
    refill_party_hearts();
    lsw2_state_request_in_slot(kLsw2WorldSlot, kLsw2LevelStateKind,
                               uint32_t(Lsw2LevelMode::CoopEnter),
                               uint32_t(kLsw2ArenaLobbyLevel), 0u);
    lsw2_state_request_in_slot(kLsw2ScreenSlot, uint32_t(kLsw2StateDefaultArm),
                               0u, 0u, 0u);
}

void lsw2_score_tick() {
    if (!ui_fork_build()) return;

    /* THE WATCH RUNS BEFORE EVERY GUARD BELOW, deliberately. The interesting
     * frames are the ones just after the press, and by then the state may have
     * changed or gone - so a report placed after the `!state` return would go
     * silent at exactly the moment it is meant to explain. That is the same
     * mistake as the instrument that could not see the thing whose absence it
     * was reporting, and this file has already paid for it once. */
    if (g_watch_left > 0) {
        --g_watch_left;
        const uint32_t local =
            bus_device_read32(0, kLsw2NetCountObject + kLsw2NetLocalStation) & 0xFFu;
        const uint32_t slot = kLsw2NetCountObject + (local & 3u) * kInputStride;
        const uint32_t buttons = bus_device_read32(0, slot) & 0xFFFFu;
        const uint32_t sel =
            g_state ? (bus_device_read32(0, (g_state + kScoreSelection) & ~3u)
                       >> (((g_state + kScoreSelection) & 3u) * 8u)) & 0xFFu
                    : 0xFFu;
        std::fprintf(stderr,
                     "[fork] score press watch: press_left=%d local=%u "
                     "buttons=%04X A=%s selection=%02X state=%08X\n",
                     g_press_left, unsigned(local), unsigned(buttons),
                     (buttons & kKeyA) ? "YES" : "no",
                     unsigned(sel), unsigned(g_state));
    }

    /* SERVE AN OWED RETURN EVERY TICK UNTIL THE GAME ACCEPTS IT. Placed above
     * the state guards because the moment it is accepted the score state goes
     * away, and a retry that lived below `if (!state) return` would stop
     * retrying at exactly the wrong time. */
    /* BOTH HALVES, BECAUSE THE PORT CAN MOVE BOTH.
     *
     * An earlier analysis concluded "the level and the state are separate and
     * the port can move only one of them", and the second half of that was
     * wrong. There are
     * two mechanisms, not one:
     *
     *   the LEVEL   lsw2_cheats_warp_cold  - writes the level request record
     *   the STATE   lsw2_wh_write_state_request - writes the state request
     *               block the way the cartridge writes it to itself, needing
     *               no guest call and no dispatch, from the port's own tick
     *
     * The second is not new and not speculative: it is how "Start Game" opens
     * the cartridge's own Game Settings, which was confirmed on screen.
     *
     * SO THE RETURN ASKS FOR THE CONDITION A CLEAN LOBBY ACTUALLY HAS. A lobby
     * carries state 48 over level 9, and 48 is the cartridge's own DEFAULT
     * ARM - what an ordinary level entry produces. The crash the old return
     * caused was state 42 over level 9, a pairing the game never
     * makes itself, so asking for the pairing it does make is the fix rather
     * than a patch over the symptom.
     *
     * AND NOTHING IS PRESSED ON THIS PATH, which is the other half of the
     * lesson. Pressing Continue is what CREATED state 42: row 0 means play
     * again, so the press started a round and the level request then yanked the
     * ground out from under it. Two measurements bracket this exactly - one
     * run pressed nothing, reached level 9, and both consoles stayed HEALTHY
     * with only the score screen left over; another pressed, reached level 9,
     * and both consoles DIED. The difference is the match state, so
     * the fix is to never make one. */
    /* THE STATE FIRST, THEN THE LEVEL - AND THAT ORDER IS THE WHOLE TRICK.
     *
     * MEASURED: asking for the level and then the state
     * left the console on **state 48, level 0**. The state was right and the
     * level had vanished, because THE TWO REQUESTS SHARE ONE BLOCK. Both write
     * eight words at `manager + slot*0x20 + 0x48`:
     *
     *   lsw2_cheats_warp_cold        [kind][arg_a][LEVEL][arg_c]
     *   lsw2_wh_write_state_request  [state id][1][0]...
     *
     * so the second write simply replaced the first. Nothing was wrong with
     * either mechanism; they were asked in an order that made one erase the
     * other.
     *
     * Reversed, each does the job the other could not. The STATE request
     * dismisses the score screen - which is what the level-only version could
     * not do, leaving state 44 sitting over the lobby. The LEVEL request then
     * lands last and wins, and a level entry produces state 48 by itself,
     * because 48 IS the cartridge's default arm. So the end condition is
     * the one a clean lobby has, arrived at the way the game arrives at it. */
    /* NOT FROM HERE IN A SHARED SESSION: the return is a write into the game's
     * state machine, and from this tick it lands at a different step on every
     * console - see lsw2_score_lobby_return_begin. */
    if (g_want_lobby && !lsw2_vwh_lockstep()) serve_lobby_return();

    /* DECREMENT AFTER REPORTING, NOT BEFORE. The first watch line used to read
     * `press_left=0` on the very frame the press was live, because the counter
     * was stepped above the print - so the instrument hid the one value it was
     * added to show. */
    if (g_press_left > 0) --g_press_left;

    /* THE READ THAT WAS WRONG, AND IS WHY THIS FEATURE NEVER WORKED.
     *
     * This used to be:
     *
     *     manager = read32(kLsw2StateManager);          <- one deref too many
     *     s       = read32(manager + kLsw2StateCurrent);
     *
     * `kLsw2StateManager` is the manager's BASE ADDRESS, not a pointer to it,
     * so the current-state pointer is one read: base + offset. The old chain
     * dereferenced the manager's first word and then indexed off whatever that
     * happened to be, so the id never equalled 0x2C, `state` stayed 0, and this
     * function returned at the `!state` branch every single frame.
     *
     * MEASURED 2026-09-08: a real match ended, the game sat in state 0x2C for
     * SIXTY seconds, and this module printed nothing at all - not even the
     * arming diagnostic added that day to tell "never seen" from "re-armed".
     * Never seen. statelog.cpp had the correct chain the whole time, four feet
     * away, which is why its trail showed the 44 this file could not find.
     *
     * Now there is ONE copy of the read and everything shares it. */
    uint32_t state = lsw2_state_current_object();
    if (state &&
        int(bus_device_read32(0, state + kLsw2StateId) & 0xFFu) != kLsw2StateScore)
        state = 0;

    g_screen_up = state != 0;

    if (!state) {
        /* GONE. Reset so the NEXT match's score screen gets its own fifteen
         * seconds - a latch that only ever set would give the first match a
         * timer and every later one a wait forever, which is the shape of bug
         * that gets reported as "it worked once". */
        g_armed = false;
        g_fired = false;
        g_state = 0;
        return;
    }

    const uint32_t now = bus_device_read32(0, kLsw2SecondsClock);

    /* A DIFFERENT OBJECT IS A DIFFERENT VISIT. Comparing the pointer as well as
     * the id matters because two score screens in a row would otherwise look
     * like one long one if the state id never changed between them. */
    if (!g_armed || state != g_state) {
        /* SAY WHY THIS ARMED, because a silent re-arm is indistinguishable from
         * a screen that was never seen - and on 2026-09-08 those two readings
         * needed telling apart with nothing but a log.
         *
         * MEASURED THAT DAY: a real match on the default path ended, the game
         * entered state 0x2C for SIXTY seconds (12:53:24 -> 12:54:24), and this
         * function printed nothing at all. The tick is reached every frame
         * (fork.cpp has no early return before it), the seconds clock advances
         * (the lobby countdown reads the same word and moved 328 counts), and
         * kLsw2StateScore is 0x2C. So the failure is HERE, and there are only
         * two shapes it can have:
         *
         *   NEVER SEEN     the manager/current/id read does not match, and the
         *                  first branch below never runs at all.
         *   RE-ARMED       `state` is not stable frame to frame, so this branch
         *                  runs forever, resetting g_entered each time and
         *                  making the fifteen seconds unreachable by
         *                  construction.
         *
         * The second is the leading hypothesis and this line is what decides
         * it. Capped, because if it IS churning it would otherwise print every
         * frame and drown the log it is meant to explain. */
        static int announced = 0;
        if (announced < 8) {
            ++announced;
            std::fprintf(stderr,
                         "[fork] score screen: %s (state %08X, was %08X, "
                         "clock %u)\n",
                         g_armed ? "RE-ARMED - the state object changed"
                                 : "seen, arming the fifteen seconds",
                         unsigned(state), unsigned(g_state), unsigned(now));
        }
        g_armed = true;
        g_fired = false;
        g_state = state;
        g_entered = now;
        return;
    }

    if (!lsw2_score_should_press(true, g_fired, now, g_entered)) return;

    /* THE ROW FIRST, THEN THE PRESS. 0x0209CB94 reads this byte and nothing
     * else, so writing it after the press would commit whatever the player had
     * highlighted instead. */
    /* THE RETURN TO THE LOBBY, WHICH IS WHAT THE FORK SPECIFIES AND WHAT NO
     * CARTRIDGE ROW DOES: once the score screen is done, everyone goes back to
     * the lobby, and an enabled lobby timer restarts.
     *
     * PRESSING CONTINUE WAS MEASURED AND IT IS THE WRONG ROW. Row 0 sends
     * message 0x0D and means *play again*: it produced `state 42, level 5` -
     * another match on the same map, immediately. None of the four rows returns
     * to a cantina lobby, because the retail wireless arena has no walkaround
     * lobby between rounds; players sit on a starfield, and replacing that
     * starfield is the whole point of the fork's lobby.
     *
     * SO THE FORK ASKS FOR THE LOBBY DIRECTLY, with the boot warp's own
     * mechanism (the one that places the player straight in the arena lobby
     * at startup). The
     * COLD warp is the one that fits: `lsw2_cheats_warp` copies two arguments
     * from a running level state, and during the score screen there is no level
     * at all (the log reads `state 44 level 0`). `lsw2_cheats_warp_cold`
     * supplies them instead and needs only the state manager to be idle, which
     * is exactly the boot case the boot warp already uses it for. Nothing is invented
     * here; this is the established route for "put the player in the arena
     * lobby", used at the one other moment the fork needs it.
     *
     * AND IT IS RETRIED RATHER THAN FIRED ONCE. The manager refuses while a
     * transition is in flight, and a refusal on the one frame we asked would
     * silently drop the return and leave the screen up - the same shape of loss
     * as the one-frame press this file just finished fixing. */
    /* BOTH, AND IN THIS ORDER - MEASURED.
     *
     * The first attempt asked for the lobby and did NOT press. The request was
     * accepted, both consoles genuinely reached level 9 - and the score screen
     * was still there, because THE LEVEL AND THE STATE ARE SEPARATE THINGS.
     * The log shows `JOIN GAP (HOST) state 44 level 9`: the lobby loaded
     * underneath a score screen that never went away, and `[state] entered 44`
     * was the last state entry of the whole run.
     *
     * So the level request moves the world and the press dismisses the screen,
     * and the return needs both. The press is restored - it is proven to
     * dismiss at fifteen seconds - and the lobby request is deferred
     * until the screen is actually gone, which is the ordering the first
     * attempt got wrong by firing while the manager happened to be idle. */
    g_fired = true;

    /* THE PRESS AND THE RETURN ARE NOW ALTERNATIVES, NOT A SEQUENCE.
     *
     * They were done together and that is what crashed. Row 0 means *play
     * again*, so pressing it CREATES a match state; the level request then
     * pulled the ground from under that match and the per-frame update walked a
     * freed list. The press is not a neutral way to close the screen - it is
     * the thing that made the fatal pairing.
     *
     * So: with the return OFF, press, and the game does what row 0 means. With
     * the return ON, press NOTHING and ask for the lobby's own level and state
     * instead - the screen is left by going somewhere, which is what "returned
     * to the lobby" means anyway. */
    /* ON BY DEFAULT, an OPT-OUT now rather than an opt-in. The flag guarded
     * the return while its host crash was open; the crash is fixed (the
     * portrait module's settling window) and two clean runs returned both
     * consoles to the lobby ALIVE with the timer re-arming. Returning all
     * players to the lobby once a match concludes is the specified default,
     * so it is what ships.
     * `LSW2_LOBBY_RETURN=0` restores the old press-Continue behaviour exactly,
     * the same opt-out shape LSW2_REAL_WH keeps, so a regression can be
     * bisected without a rebuild. */
    static const bool want_return = [] {
        const char* e = std::getenv("LSW2_LOBBY_RETURN");
        return !(e && *e == '0');
    }();

    /* A SHARED SESSION RETURNS TOGETHER OR NOT AT ALL.
     *
     * Each console's seconds clock is its own: four consoles entered this
     * screen at the same step reading 2904909546, ...506, ...499 and ...492. So
     * four timers fired at four moments, and each console rewrote its own state
     * machine from this tick - two reloaded the lobby a minute apart while two
     * sat here, and the lobby that loaded under a split simulation fed the
     * sprite loader a source that is not data and wedged the geometry engine.
     *
     * So in a session only the HOST's timer counts, and what it does is stamp
     * the return on a dataset (vwh.h, kLsw2EffectLobbyReturn). Every console,
     * the host included, starts it when it consumes that dataset -
     * lsw2_score_lobby_return_begin - and serves it at each dataset boundary
     * until the game has taken both halves. A client's own timer does nothing.
     * A refused post (the effect queue full) un-fires, so the next tick retries. */
    if (want_return && lsw2_vwh_lockstep()) {
        if (lsw2_vwh_hosting()) {
            if (lsw2_vwh_post_effect(kLsw2EffectLobbyReturn, nullptr, 0))
                std::fprintf(stderr, "[fork] score screen: 15s on the host's "
                             "clock - the lobby return is stamped on the next "
                             "dataset for every console\n");
            else
                g_fired = false;
        }
        return;
    }

    if (!want_return) {
        write_guest_byte(g_state + kScoreSelection, kRowContinue);
        g_press_left = kPressFrames;
    }

    /* THE LOBBY RETURN WAS ONCE OFF BY DEFAULT, AND THAT WAS A RETREAT.
     *
     * It worked: a measured run had both consoles reaching level 9 and the
     * in_match latch producing its first-ever falling edge. And it HANGS THE
     * GUEST. Two runs, measured, with the crash attributed rather than guessed:
     *
     *   run with post-return console driving   both consoles dead
     *   run WITHOUT it (the discriminator)     both consoles dead, identically
     *
     * Same signature both times - 28 unmapped accesses, `dispatch-miss
     * pc=0x02154510`, and the known double free in FUN_02055210 walking a
     * linked list whose pointers are ASCII text. The second run is what
     * convicts the feature rather than the instrument: it drove nothing after
     * the return, so LSW2_TEST_CONSOLE cannot be blamed.
     *
     * WHY IT CRASHES. Pressing Continue starts another round - `state 42, level
     * 5` - and the deferred request then yanks the level to 9 out from under
     * it. A clean lobby carries state 15, 35, 48 or -1 at level 9; **42 over 9
     * is a combination the game never produces itself**, and the per-frame
     * update walks a list belonging to the match that was torn down beneath it.
     *
     * WHY IT WAS NOT SIMPLY FIXED HERE. The belief at the time was that the
     * level and the state are separate and the port can move only one of
     * them. Moving the state too means running the game's own transition,
     * which waits on WH_End and completes only on the guest's thread. That is
     * the same wall the lobby respawn on Start Lobby is behind, and the
     * trampoline is the way through it for both.
     *
     * THE DEFAULT WAS ONCE THE BEHAVIOUR THAT DOES NOT CRASH: press Continue,
     * another round, the specified return of everyone to the lobby left
     * undone but the host alive. The fix that fulfils it asks for the
     * lobby's STATE as well as its level, the pairing a clean lobby has rather
     * than the one that crashed - and a later fix removed the LAST crash in it
     * (the portrait rebuild after the return). Two clean runs later the
     * return IS the default; the opt-out above is all that remains of the
     * flag. */
    if (want_return) g_want_lobby = true;
    /* WATCH WHETHER THE GUEST EVER SEES THE A - the next N ticks report the
     * input manager's own button word, and the selection byte read back.
     *
     * WHY THIS AND NOT A LONGER PRESS. It was measured that the timer fires
     * and the screen is still dismissed by the game's own timeout at exactly
     * the same moment as in the two runs where nothing pressed anything. So
     * something between "we set the bit" and "the game acts on it" is losing
     * it, and there are two candidates that need telling apart before either is
     * changed:
     *
     *   THE PRESS IS TOO SHORT   kPressFrames is 1, and this project has twice
     *                            recorded that a one-frame tap is silently
     *                            dropped (tools/pilot.py needs --frames for
     *                            exactly this reason). The guest may sample on
     *                            a different edge than the port's frame.
     *   THE PRESS NEVER ARRIVES  the score state may not read the pad the port
     *                            writes at all.
     *
     * The manager at 0x021027F8 is where FUN_0205ce64 publishes the pad, so if
     * A never appears in the local slot's halfword the injection is not
     * reaching the guest and lengthening it would change nothing.
     *
     * AND THE SELECTION BYTE IS READ BACK because writing it may be pointless:
     * kRowContinue is 0 and the state's own enter already sets 0, so that write
     * is a no-op unless something moved the cursor. Worth confirming rather
     * than assuming - it decides whether the byte is even part of this. */
    g_watch_left = 10;

    std::fprintf(stderr,
                 "[fork] score screen: %ds elapsed - dismissing, then returning "
                 "to the lobby (state %08X)\n",
                 kLsw2ScoreSeconds, unsigned(g_state));
}

/* See scorescreen.h. One reader, so "is the score screen up" cannot drift from
 * the answer lsw2_score_tick acts on. */
bool lsw2_score_screen_is_up() {
    const uint32_t s = lsw2_state_current_object();
    return s && int(bus_device_read32(0, s + kLsw2StateId) & 0xFFu)
                    == kLsw2StateScore;
}

uint16_t lsw2_score_auto_keys() {
    /* RELEASED THE MOMENT THE SCREEN IS GONE, which is what makes a long hold
     * safe. The old one-frame press existed to guarantee the A could not leak
     * into whatever came next; gating on the screen still being up gives that
     * guarantee outright, so the hold no longer has to be short enough to be
     * unreliable. g_screen_up is refreshed by the tick from the same shared
     * state read everything else uses. */
    return (g_press_left > 0 && g_screen_up) ? kKeyA : 0u;
}
