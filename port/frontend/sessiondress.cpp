/* See sessiondress.h. */

#include "sessiondress.h"

#include "cheats.h"          /* lsw2_cheats_party_actor */
#include "customization.h"   /* lsw2_customization_effective, kLsw2CustomNone */
#include "gearhop.h"
#include "netcount.h"        /* kLsw2NetCountObject, kLsw2NetLocalStation */
#include "ui.h"
#include "vwh.h"

#include "state.h"

#include <cstdio>

namespace {

/* The customize menu's own per-player helpers - see sessiondress.h. */
constexpr uint32_t kHatHelper = 0x0207B240u;      /* (_, actor, slot, part)  */
constexpr uint32_t kWeaponHelper = 0x0207B2C8u;   /* (_, actor, row)         */

/* The message carries the part as a BYTE (0x0207B288 `STRB r3, [sp, #2]`), so a
 * part id past it could not travel; none of the customize parts is. */
constexpr uint16_t kLargestSendable = 0xFFu;

/* A NEW ACTOR SETTLES BEFORE IT IS DRESSED. A spawn builds the actor and then
 * dresses it with the character's stock parts over the next frames (0x02001D20);
 * asking first would have the helpers compare against a half-built figure. Half
 * a second, the settling window hudportrait.cpp measured for the same actor. */
constexpr int kSettleFrames = 30;

uint32_t g_actor = 0;
int g_settle = 0;
bool g_dressed = false;

uint8_t local_station() {
    const uint32_t a = kLsw2NetCountObject + kLsw2NetLocalStation;
    const uint32_t w = bus_device_read32(0, a & ~3u);
    return uint8_t((w >> ((a & 3u) * 8u)) & 0xFFu);
}

}  // namespace

void lsw2_sessiondress_tick() {
    if (!ui_fork_build() || !lsw2_vwh_lockstep()) {
        g_actor = 0;
        g_dressed = false;
        return;
    }

    /* ONLY ONCE THE GAME AGREES WHO THIS CONSOLE IS, AND WILL SEND.
     *
     * Measured: mid-join the game's station byte still read 0 on a joiner, and
     * this dressed station 0's actor - the HOST's - while the multi-view bit
     * that makes the helpers send a message instead of acting locally may not
     * be up yet. A local dressing on one console is exactly the divergence the
     * whole module exists to avoid. So: the game's station must be this
     * console's session AID, and the bit must be set. */
    const uint8_t station = local_station();
    if (station >= 4u || int(station) != lsw2_vwh_local_aid() ||
        !lsw2_vwh_multiview())
        return;
    const uint32_t actor = lsw2_cheats_party_actor(int(station));
    if (actor < 0x02000000u || actor >= 0x02400000u) {
        g_actor = 0;
        return;
    }

    /* A DIFFERENT ACTOR IS A NEW FIGURE - a spawn, an arena entry, a switch -
     * and every one of those starts from the character's stock parts. */
    if (actor != g_actor) {
        g_actor = actor;
        g_settle = kSettleFrames;
        g_dressed = false;
        return;
    }
    if (g_dressed) return;
    if (g_settle > 0) { --g_settle; return; }

    uint16_t want[kLsw2CustomSlots];
    lsw2_customization_effective(want);

    int asked = 0;
    for (int slot = 0; slot < kLsw2CustomSlots - 1; ++slot) {
        if (want[slot] == kLsw2CustomNone || want[slot] > kLargestSendable) continue;
        if (lsw2_gearhop_request_action(kHatHelper, 0u, actor, uint32_t(slot),
                                        want[slot]))
            ++asked;
    }
    const uint16_t row = want[kLsw2CustomSlots - 1];
    if (row != kLsw2CustomNone && row <= kLargestSendable &&
        lsw2_gearhop_request_action(kWeaponHelper, 0u, actor, row, 0u))
        ++asked;

    g_dressed = true;
    if (asked > 0)
        std::fprintf(stderr, "[fork] session dress: station %u's actor %08X - %d "
                     "of its saved parts sent through the game's own customize "
                     "messages (every console dresses it at the same dataset)\n",
                     unsigned(station), unsigned(actor), asked);
}
