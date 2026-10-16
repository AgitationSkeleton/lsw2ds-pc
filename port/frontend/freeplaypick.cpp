/* See freeplaypick.h. */

#include "freeplaypick.h"

#include "cheats.h"          /* lsw2_cheats_party_actor, kLsw2ActorCharacter */
#include "forksession.h"
#include "hostsettings.h"    /* Lsw2Roster */
#include "levels.h"          /* kLsw2ArenaLobbyLevel */
#include "lobbyhud.h"        /* kLsw2SecondsClock */
#include "selectwidget.h"
#include "statelog.h"        /* kLsw2StateSelect, Lsw2LevelMode, the slots */
#include "statereq.h"        /* lsw2_state_request_in_slot */
#include "ui.h"
#include "viewmask.h"
#include "vwh.h"

#include "state.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_02086534(void);

namespace {

/* THE UNLOCK QUERY AND THE ONE CALL OF IT THAT IS A TAP:
 *     0206887C  bl 0x02086534     the tapped character - returns to 02068880
 *     020688B4  bl 0x02086534     the previous selection's, to un-highlight it */
constexpr uint32_t kUnlockQuery = 0x02086534u;
constexpr uint32_t kTapCheckReturn = 0x02068880u;

/* THE ROUND START'S TWO REQUESTS, as statereq.cpp's log names their callers
 * (lr): the Walkaround world, then the select (FUN_02072CF4, 0x02072DA4). */
constexpr uint32_t kRoundStartWorld = 0x02072DC8u;
constexpr uint32_t kRoundStartScreen = 0x02072DE4u;

/* The same timer as usual - the statue select's 30 s. */
constexpr int kPickSeconds = 30;

/* The Free Play switch in a session (0x020B1524 -> 0x020917C8). */
constexpr int kMsgFreePlaySwitch = 7;

/* How long the host lets the last switch land before it reads the picks:
 * about a second and a half of ticks, the settling window hudportrait.cpp
 * measured for the same actor rebuild. */
constexpr int kSettleTicks = 90;

/* DS keys, the four this needs. */
constexpr uint16_t kKeyA = 1u << 0;
constexpr uint16_t kKeyB = 1u << 1;
constexpr uint16_t kKeyR = 1u << 8;
constexpr uint16_t kKeyL = 1u << 9;

/* WHERE A PORTRAIT IS, to tap it: the measured grid is eight 40x40
 * composites, 4 across and 2 down, the first at (37, 53), pitch 48 both ways -
 * so an entry's centre is (57 + 48*col, 73 + 48*row). */
constexpr int kGridX0 = 57;
constexpr int kGridY0 = 73;
constexpr int kGridPitch = 48;
constexpr int kGridColumns = 4;

/* The synthetic tap: pressed for four frames, released for two - the shape
 * tools/pilot.py's `--frames` note says a tap needs to register. */
constexpr int kTapPressFrames = 4;
constexpr int kTapFrames = kTapPressFrames + 2;

enum class Mode { None, Host, PreGame };
Mode g_mode = Mode::None;

int g_host_pick = -1;
bool g_host_cancel = false;

bool g_locked = false;          /* this console's player has confirmed */
uint32_t g_start_clock = 0;     /* the game's seconds clock at the start */
bool g_start_screen_owed = false; /* the world was rewritten; the 41 is next */
unsigned g_confirmed = 0;       /* host: stations that sent a switch */
int g_settle = 0;               /* host: ticks until the last switch has landed */
bool g_end_posted = false;      /* host: the end effect is out */
int g_round_pick[4] = {-1, -1, -1, -1};

int g_tap_frames = 0;
uint16_t g_tap_x = 0, g_tap_y = 0;

uint8_t rd8(uint32_t a) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint8_t((w >> ((a & 3u) * 8u)) & 0xFFu);
}

void wr8(uint32_t a, uint8_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t sh = (a & 3u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

bool ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

uint32_t seconds_now() { return bus_device_read32(0, kLsw2SecondsClock); }

/* THE SEATED STATIONS, as the game's own view record 0 holds them - a mask,
 * not a count. A player who left mid-session leaves a gap (0b101 after
 * station 1 went), and a count of 2 would have waited on the departed
 * station 1 and never read station 2's pick. */
unsigned seated_mask() {
    const int mask = lsw2_viewmask_guest();
    return mask > 0 ? unsigned(mask) & ((1u << kLsw2ViewRecords) - 1u) : 0u;
}

bool seated(unsigned mask, int station) { return ((mask >> station) & 1u) != 0; }

bool pregame_wanted() {
    return ui_fork_build() && lsw2_vwh_lockstep() &&
           lsw2_fork_session().settings().roster == Lsw2Roster::All;
}

void wr16(uint32_t a, uint16_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t sh = (a & 2u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
}

/* Each seated player's record, made to say what that player is now - on the
 * guest's thread, inside the round start every console runs at one step, so
 * every console writes the same bytes. */
void keep_party_through_reload() {
    const unsigned mask = seated_mask();
    for (int i = 0; i < 4; ++i) {
        if (!seated(mask, i)) continue;
        const uint32_t actor = lsw2_cheats_party_actor(i);
        if (!ram(actor)) continue;
        const uint32_t rec =
            kLsw2AppearanceRecord + uint32_t(i) * kLsw2AppearanceStride;
        wr16(rec, rd8(actor + kLsw2ActorCharacter));
        wr8(rec + kLsw2AppearanceGate, 1u);
        wr8(rec + kLsw2AppearanceHearts, kLsw2PlayerBuiltHearts);
    }
}

}  // namespace

/* --- 1. The host's One character ------------------------------------------ */

void lsw2_freeplaypick_host_begin() {
    if (g_mode == Mode::PreGame) return;
    g_mode = Mode::Host;
    g_host_pick = -1;
    g_host_cancel = false;
    std::fprintf(stderr, "[fork] host picker: the Free Play grid picks the One "
                 "character - double-tap to choose, B to go back\n");
}

void lsw2_freeplaypick_host_end() {
    if (g_mode == Mode::Host) g_mode = Mode::None;
}

bool lsw2_freeplaypick_host_active() { return g_mode == Mode::Host; }

int lsw2_freeplaypick_host_take() {
    const int c = g_host_pick;
    g_host_pick = -1;
    return c;
}

bool lsw2_freeplaypick_host_cancelled() {
    const bool c = g_host_cancel;
    g_host_cancel = false;
    return c;
}

/* --- 2. The pre-game pick under ALL ----------------------------------------- */

void lsw2_freeplaypick_round_start_request() {
    const uint32_t caller = g_cpu.R[14];
    if (caller == kRoundStartWorld && pregame_wanted()) {
        /* EVERY CONSOLE, THE SAME STEP: the round start is the game's own
         * handling of the host's start message, so this runs at one exchange
         * on all of them, and the pick begins there. */
        g_mode = Mode::PreGame;
        g_locked = false;
        g_start_clock = seconds_now();
        g_start_screen_owed = true;
        g_confirmed = 0;
        g_settle = 0;
        g_end_posted = false;
        for (int& p : g_round_pick) p = -1;

        /* THE LOBBY IS RELOADED FOR THE PICK, AS THE LOBBY RETURN RELOADS IT
         * (world CoopEnter + screen 48, scorescreen.cpp). Keeping the world
         * and only swapping the screen left the host's top screen on the
         * rules screen's map picture - 54's own display, which the select
         * that normally replaces it would have taken over (conn_allpick). The
         * reload restores each gated record into its player, so first the
         * records get what the players are NOW - their Free Play characters,
         * which the records never saw, and the hearts every player is built
         * with (see scorescreen.cpp's refill_party_hearts for the 0xFF the
         * record otherwise holds). */
        keep_party_through_reload();
        g_cpu.R[3] = uint32_t(Lsw2LevelMode::CoopEnter);
        std::fprintf(stderr, "[fork] ALL: the character select is the Free Play "
                     "grid this round - the lobby reloads for a %d s pick "
                     "instead of the Walkaround world\n", kPickSeconds);
        return;
    }
    if (caller == kRoundStartScreen && g_start_screen_owed &&
        g_cpu.R[1] == uint32_t(kLsw2StateSelect)) {
        /* 48, NOT 41: the lobby's own screen state, whose tab UI holds the grid
         * - on the host that also closes the rules screen the 41 would have
         * replaced. */
        g_start_screen_owed = false;
        g_cpu.R[1] = uint32_t(kLsw2StateDefaultArm);
    }
}

bool lsw2_freeplaypick_pregame_active() { return g_mode == Mode::PreGame; }

int lsw2_freeplaypick_pregame_seconds_left() {
    if (g_mode != Mode::PreGame) return -1;
    const int gone = int(seconds_now() - g_start_clock);
    const int left = kPickSeconds - gone;
    return left < 0 ? 0 : left;
}

void lsw2_freeplaypick_pregame_end() {
    if (g_mode != Mode::PreGame) return;
    g_mode = Mode::None;
    g_start_screen_owed = false;
    /* THE ROUND START THE PICK STOOD IN FOR, as the cartridge asks for it
     * (0x02072DC4 / 0x02072DE0): the Walkaround world, then the select. */
    lsw2_state_request_in_slot(kLsw2WorldSlot, kLsw2LevelStateKind,
                               uint32_t(Lsw2LevelMode::Walkaround),
                               uint32_t(kLsw2ArenaLobbyLevel), 0u);
    lsw2_state_request_in_slot(kLsw2ScreenSlot, uint32_t(kLsw2StateSelect),
                               0u, 0u, 0u);
    std::fprintf(stderr, "[fork] ALL: the pick is over at this dataset - the "
                 "round start runs now (Walkaround world, then the select)\n");
}

int lsw2_freeplaypick_round_pick(int station) {
    return (station >= 0 && station < 4) ? g_round_pick[station] : -1;
}

void lsw2_freeplaypick_note_message(int station, int type) {
    if (g_mode != Mode::PreGame || type != kMsgFreePlaySwitch) return;
    if (station < 0 || station >= 4) return;
    if (!(g_confirmed & (1u << station)))
        std::fprintf(stderr, "[fork] ALL: station %d confirmed a character\n",
                     station);
    g_confirmed |= 1u << station;
    g_settle = kSettleTicks;
}

/* --- Both ------------------------------------------------------------------ */

void lsw2_freeplaypick_tick() {
    if (g_mode != Mode::PreGame || g_end_posted || !lsw2_vwh_hosting()) return;

    /* THE HOST ENDS IT: the select's 30 seconds, or every seated player has
     * confirmed - the same early end the statue select has when the last
     * player chooses. */
    const unsigned everyone = seated_mask();
    const bool all_in = everyone != 0u && (g_confirmed & everyone) == everyone;

    /* NOT THE FRAME THE LAST SWITCH ARRIVES: a Free Play switch rebuilds the
     * player's actor, and read at once the slot was empty (-1) or still the
     * old figure (conn_allpick). The last confirm is given time to land. */
    if (g_settle > 0) --g_settle;
    if (lsw2_freeplaypick_pregame_seconds_left() > 0 && (!all_in || g_settle > 0))
        return;

    /* THE PICKS, READ HERE IN THE LOBBY - every switch reached every console
     * as message 7, so the host's copy of each actor is everyone's. After the
     * round start's Walkaround reload each actor is character 0. A seated
     * player whose actor is mid-rebuild holds the end back a frame. */
    for (int i = 0; i < 4; ++i)
        if (seated(everyone, i) && !ram(lsw2_cheats_party_actor(i))) return;
    for (int i = 0; i < 4; ++i) {
        const uint32_t actor = lsw2_cheats_party_actor(i);
        g_round_pick[i] = (seated(everyone, i) && ram(actor))
                              ? int(rd8(actor + kLsw2ActorCharacter)) : -1;
    }
    if (!lsw2_vwh_post_effect(kLsw2EffectPickEnd, nullptr, 0)) return;
    g_end_posted = true;
    std::fprintf(stderr, "[fork] ALL: %s - the picks are %d %d %d %d\n",
                 all_in ? "every player has confirmed" : "the 30 s are up",
                 g_round_pick[0], g_round_pick[1], g_round_pick[2],
                 g_round_pick[3]);
}

uint16_t lsw2_freeplaypick_filter_keys(uint16_t keys) {
    static uint16_t prev = 0;
    const uint16_t fresh = keys & uint16_t(~prev);
    prev = keys;

    if (g_mode == Mode::Host) {
        /* NO A-CONFIRM HERE: this picker uses the normal Free Play touch
         * menu controls and logic (characterpick.h) - L and R page, a double
         * tap confirms, B backs out. A is the pre-game pick's, not this one's. */
        if (fresh & kKeyB) g_host_cancel = true;
        return keys & uint16_t(kKeyL | kKeyR);
    }
    if (g_mode == Mode::PreGame) {
        if ((fresh & kKeyA) && !g_locked && g_tap_frames == 0) {
            const int entry = lsw2_selectwidget_entry();
            if (entry >= 0) {
                /* A CONFIRMS BY TAPPING THE HIGHLIGHTED PORTRAIT AGAIN - the
                 * widget's own second tap, so the switch is the game's. */
                g_tap_x = uint16_t(kGridX0 + kGridPitch * (entry % kGridColumns));
                g_tap_y = uint16_t(kGridY0 + kGridPitch * (entry / kGridColumns));
                g_tap_frames = kTapFrames;
            }
        }
        return keys & uint16_t(~(kKeyA | kKeyB));
    }
    return keys;
}

int lsw2_freeplaypick_touch(uint16_t* x, uint16_t* y) {
    if (g_tap_frames <= 0) return 0;
    --g_tap_frames;
    if (g_tap_frames >= kTapFrames - kTapPressFrames) {
        if (x) *x = g_tap_x;
        if (y) *y = g_tap_y;
        return 1;
    }
    return 2;
}

void lsw2_freeplaypick_unlock_query() {
    if (g_mode != Mode::None && g_cpu.R[14] == kTapCheckReturn) {
        const uint32_t widget = g_cpu.R[6];
        const uint8_t entry = uint8_t(g_cpu.R[5] & 0xFFu);
        const int character = int(g_cpu.R[1]);
        const bool confirming =
            ram(widget) && rd8(widget + kLsw2SelectEntry) == entry;

        if (g_mode == Mode::Host && confirming) {
            /* THE HOST'S PICK, AND NO SWITCH: with the selection cleared the
             * handler's compare at 0x02068908 sees a first tap, highlights the
             * portrait and returns. */
            g_host_pick = character;
            wr8(widget + kLsw2SelectEntry, kLsw2SelectNone);
        } else if (g_mode == Mode::PreGame) {
            if (g_locked) {
                /* LOCKED: the widget hears "not unlocked" and refuses the tap
                 * with its own error sound (0x02068888 -> sound 0xB7) - AND
                 * its selection is cleared, because the refusal alone does not
                 * stop a confirm. The "not unlocked" path FALLS THROUGH to the
                 * selection compare at 0x02068908, and the caller records the
                 * tapped entry in +0x36 whatever the answer (0x020685DC), so a
                 * second tap on a refused portrait confirmed it anyway -
                 * measured (conn_allpick): locked in as 61, then switched to
                 * 64. With the selection cleared the compare never matches. */
                if (ram(widget)) wr8(widget + kLsw2SelectEntry, kLsw2SelectNone);
                g_cpu.R[0] = 0u;
                g_cpu.R[15] = g_cpu.R[14];
                return;
            }
            if (confirming) {
                g_locked = true;
                std::fprintf(stderr, "[fork] ALL: this player locked in as "
                             "character %d\n", character);
            }
        }
    }
    /* THE GAME'S OWN QUERY, as the override's last act. */
    g_cpu.R[15] = kUnlockQuery;
    lsw2_arm9_FUN_02086534();
}
