/* The guest half of the arena's select skip - see selectskip.h. */

#include "selectskip.h"
#include "vwh.h"
#include "statelog.h"
#include "viewmask.h"

#include "cheats.h"
#include "forksession.h"
#include "roster.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* The state manager, and the player count the select compares against.
 *
 * `0x02073034` returns `[0x020FEB88]` and the select's update uses it as the
 * player count at six sites. Reading the same word rather than
 * asking the fork's session is deliberate: the comparison that ends the select
 * is `+0x2B06 == that`, so matching the game's own number is what makes the
 * state finish. The session's count is the fork's opinion and could differ by
 * one during a join. */
/* See statelog.h - kLsw2StateManager is the one copy. The count word is
 * viewmask.h's kLsw2PumpCountWord; this file used to declare it again as
 * kPlayerCountWord. Merging the two is what turned up viewmask.h's own
 * reference table calling +0 "the game bound" when +0 is the count. */

bool g_done = false;
int g_last_kind = -1;

bool ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

void write_byte(uint32_t addr, uint8_t v) {
    const uint32_t base = addr & ~3u;
    const uint32_t sh = (addr & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

/* The live state object for a slot, or 0. */
uint32_t state_object(int slot) {
    if (slot < 0 || slot > 1) return 0;
    const uint32_t p = bus_device_read32(0, kLsw2StateManager + uint32_t(slot) * 4u);
    return ram(p) ? p : 0;
}

}  // namespace

void lsw2_selectskip_tick() {
    if (!ui_fork_build()) return;
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): these writes from the tick land at a
     * different step on each console. In a session the select is completed at
     * the dataset boundary instead - sessionroster.cpp. */
    if (lsw2_vwh_lockstep()) return;
    if (!lsw2_cheats_available()) return;

    /* WHICH SLOT HOLDS THE SELECT. Two slots, and which one a state lands in is
     * not fixed - the fork reads the kind rather than assuming, the same way
     * the pause menu's liveness check does. */
    int slot = -1;
    for (int i = 0; i < 2; ++i) {
        if (lsw2_cheats_state_kind(i) == kLsw2StateSelect) { slot = i; break; }
    }

    const int kind = slot >= 0 ? kLsw2StateSelect : -1;

    /* LEAVING THE SELECT REARMS IT. Without this the fork would skip the first
     * select of a session and no other, which is the shape of bug this project
     * has produced before - a one-shot latch that outlives the thing it
     * describes. */
    if (kind != g_last_kind) {
        g_last_kind = kind;
        if (kind != kLsw2StateSelect) g_done = false;
    }

    const bool skips =
        lsw2_roster_skips_select(lsw2_fork_session().settings().roster);

    if (!lsw2_selectskip_should(/*in_fork=*/true, skips, kind, g_done)) return;

    const uint32_t obj = state_object(slot);
    if (!obj) return;

    /* THE GAME'S OWN COUNT, not the session's - see kLsw2PumpCountWord. */
    uint32_t players = bus_device_read32(0, kLsw2PumpCountWord);
    if (players < 1u || players > uint32_t(kLsw2SelSlots)) players = 1u;

    for (uint32_t i = 0; i < players; ++i)
        write_byte(obj + kLsw2SelChosen + i, 1u);
    write_byte(obj + kLsw2SelPicked, uint8_t(players));

    g_done = true;
    std::fprintf(stderr,
                 "[fork] roster is not Hunters - marked %u player(s) chosen so "
                 "the statue select completes itself\n", unsigned(players));
}
