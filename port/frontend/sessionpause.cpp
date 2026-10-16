/* See sessionpause.h. */

#include "sessionpause.h"

#include "cheats.h"          /* lsw2_cheats_state_kind */
#include "netcount.h"        /* kLsw2NetCountObject, kLsw2NetLocalStation */
#include "gearhop.h"         /* lsw2_gearhop_request_send */
#include "sessionpause_policy.h"
#include "statelog.h"        /* kLsw2ScreenSlot, lsw2_state_pending */
#include "statereq.h"        /* lsw2_state_request_in_slot */
#include "ui.h"              /* ui_fork_build */
#include "vwh.h"             /* lsw2_vwh_lockstep */

#include "state.h"           /* bus_device_read32 / write32 */

#include <cstdio>

namespace {

/* THE SAMPLER'S INPUT WORDS, one 10-byte record a station (FUN_0205ce64 walks
 * them with `add r4, r4, #0xa`; FUN_02059e64 keeps held/prev/pressed/released
 * as four halfwords), then +0x29 the player count and +0x2A this console's
 * station - the byte the pause menu's own input helpers (FUN_0205cfe8,
 * FUN_0205d0d0) and the quit arm (0x020B2E7C) read. */
constexpr uint32_t kInputBase = kLsw2NetCountObject;
constexpr uint32_t kInputStride = 10u;
constexpr uint32_t kLocalStation = kLsw2NetCountObject + kLsw2NetLocalStation;
constexpr int kStations = 4;

/* The pause state (statelog.h). */
constexpr int kPauseState = kLsw2StatePause;

uint8_t g_paused = 0;       /* per station: the world must not see its input */
uint8_t g_zeroed_now = 0;   /* zeroed by this frame's pump override          */
uint8_t g_was_paused = 0;   /* owed its true edges on the first frame back   */
Lsw2PadState g_saved[kStations] = {};
uint16_t g_last_real_held[kStations] = {};
bool g_resume_sent = false;

bool active() { return ui_fork_build() && lsw2_vwh_lockstep(); }

uint16_t rd16(uint32_t a) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint16_t(w >> ((a & 2u) * 8u));
}

void wr16(uint32_t a, uint16_t v) {
    const uint32_t base = a & ~3u;
    const uint32_t sh = (a & 2u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
}

uint8_t rd8(uint32_t a) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint8_t(w >> ((a & 3u) * 8u));
}

Lsw2PadState read_pad(int s) {
    const uint32_t a = kInputBase + uint32_t(s) * kInputStride;
    return Lsw2PadState{rd16(a), rd16(a + 2u), rd16(a + 4u), rd16(a + 6u)};
}

void write_pad(int s, const Lsw2PadState& p) {
    const uint32_t a = kInputBase + uint32_t(s) * kInputStride;
    wr16(a, p.held);
    wr16(a + 2u, p.prev);
    wr16(a + 4u, p.pressed);
    wr16(a + 6u, p.released);
}

int local_station() {
    const int s = rd8(kLocalStation);
    return (s >= 0 && s < kStations) ? s : -1;
}

/* This console's pause state is up, or asked for and still being built (the
 * request sits in the screen slot's block through the transition's fade). */
bool local_menu_up() {
    return lsw2_cheats_state_kind(int(kLsw2ScreenSlot)) == kPauseState ||
           lsw2_state_pending() == kPauseState;
}

void reset() {
    g_paused = 0;
    g_zeroed_now = 0;
    g_was_paused = 0;
    g_resume_sent = false;
}

}  // namespace

void lsw2_sessionpause_after_sampler() {
    g_zeroed_now = 0;
    if (!active()) {
        if (g_paused || g_was_paused) reset();
        return;
    }
    for (int s = 0; s < kStations; ++s) {
        const uint8_t bit = uint8_t(1u << s);
        if (g_paused & bit) {
            /* THE MENU'S WORD, WITH TRUE EDGES. The sampler computed this
             * frame's edges against the word we zeroed last frame, so every
             * held key reads as a fresh press - measured (conn_pause): one Down
             * raced the cursor round the menu every frame it was held. The
             * menu gets them against the last real held keys instead. */
            const Lsw2PadState real = read_pad(s);
            g_saved[s] = lsw2_pause_resume_edges(real.held, g_last_real_held[s]);
            g_last_real_held[s] = real.held;
            write_pad(s, Lsw2PadState{});
            g_zeroed_now = uint8_t(g_zeroed_now | bit);
            g_was_paused = uint8_t(g_was_paused | bit);
        } else if (g_was_paused & bit) {
            const Lsw2PadState now = read_pad(s);
            write_pad(s, lsw2_pause_resume_edges(now.held, g_last_real_held[s]));
            g_was_paused = uint8_t(g_was_paused & ~bit);
        }
    }
}

bool lsw2_sessionpause_message(uint32_t msg, int station) {
    if (!active()) return false;
    const uint8_t type = rd8(msg + 1u);
    if (type != kLsw2MsgPauseOpen && type != kLsw2MsgPauseResume && type != kLsw2MsgPauseQuit)
        return false;

    const int local = local_station();
    const bool menu_up = local_menu_up();
    const Lsw2PauseMsgAction a =
        lsw2_pause_message_policy(type, station, local, menu_up, &g_paused);

    static int said = 0;
    if (said < 40) {
        ++said;
        std::fprintf(stderr,
                     "[fork] pause message 0x%02X from station %d (this console "
                     "%d, menu %s): %s - paused stations now %X\n",
                     unsigned(type), station, local, menu_up ? "up" : "down",
                     a == Lsw2PauseMsgAction::Arm      ? "the cartridge's arm"
                     : a == Lsw2PauseMsgAction::Drop   ? "dropped here"
                                                       : "this console's own menu, "
                                                         "with nothing frozen",
                     unsigned(g_paused));
    }

    switch (a) {
    case Lsw2PauseMsgAction::Arm:
        return false;
    case Lsw2PauseMsgAction::Drop:
        return true;
    case Lsw2PauseMsgAction::OpenLocal:
        /* FUN_0208ce8c's request, exactly - 0x0205ECC4(m, 0x35, 1, 0, 0, 0) -
         * without its FUN_02093c9c(_, 1). */
        lsw2_state_request_in_slot(kLsw2ScreenSlot, uint32_t(kPauseState), 0u,
                                   0u, 0u);
        return true;
    }
    return false;
}

void lsw2_sessionpause_menu_update() {
    if (!active()) return;
    const int local = local_station();
    if (local < 0) return;
    const uint8_t bit = uint8_t(1u << local);
    if (!(g_zeroed_now & bit)) return;
    write_pad(local, g_saved[local]);
}

void lsw2_sessionpause_before_entities() {
    if (!active()) return;
    for (int s = 0; s < kStations; ++s) {
        const uint8_t bit = uint8_t(1u << s);
        if (!(g_paused & bit)) continue;
        if (!(g_zeroed_now & bit)) {
            /* MARKED DURING THIS FRAME'S PUMP: the word is still the real one,
             * so it is the last real keys the resume edges start from. */
            g_last_real_held[s] = read_pad(s).held;
            g_was_paused = uint8_t(g_was_paused | bit);
        }
        write_pad(s, Lsw2PadState{});
    }
}

void lsw2_sessionpause_members(uint16_t members) {
    const uint8_t keep = uint8_t(members & ((1u << kStations) - 1u));
    if (g_paused & ~keep)
        std::fprintf(stderr, "[fork] a paused station left the session - "
                     "unmarked (paused %X, members %X)\n", unsigned(g_paused),
                     unsigned(members));
    g_paused = uint8_t(g_paused & keep);
}

void lsw2_sessionpause_tick() {
    if (!active()) {
        g_resume_sent = false;
        return;
    }
    const int local = local_station();
    if (local < 0) return;
    const uint8_t bit = uint8_t(1u << local);
    if (!(g_paused & bit)) {
        g_resume_sent = false;
        return;
    }
    if (g_resume_sent || local_menu_up()) return;
    /* OUR MENU IS GONE and every console still has us paused: a transition
     * replaced it here as it replaced the screen everywhere. The game's own
     * Resume unmarks us everywhere at one dataset, and is dropped here because
     * there is no menu left to close. */
    if (lsw2_gearhop_request_send(kLsw2MsgPauseResume, 0u, 0u)) {
        g_resume_sent = true;
        std::fprintf(stderr, "[fork] this console's pause menu was closed by a "
                     "transition - sending the game's own Resume so every "
                     "console unmarks station %d\n", local);
    }
}

bool lsw2_sessionpause_local_menu_up() {
    return lsw2_cheats_state_kind(int(kLsw2ScreenSlot)) == kPauseState;
}
