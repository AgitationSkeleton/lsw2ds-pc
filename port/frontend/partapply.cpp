/* See partapply.h. One guest call, measured. */

#include "partapply.h"
#include "vwh.h"

#include "customization.h"
#include "cheats.h"
#include "gearapply.h"
#include "guestcall.h"
#include "partslot.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_020063d4(void);

namespace {

/* See gearapply.h - kLsw2GearApplier is the one copy. */

/* Mix N Match's six parts - the player's customize choices. */
/* See customization.h for the base and the slot offsets. */

bool g_armed = false;

uint16_t read16(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint16_t((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

/* THE CALL, now through the shared helper. This file carried the whole
 * discipline inline when it was the port's only host-initiated guest call;
 * countboot.cpp is the second, and two copies of a rule about saving CPU state
 * and balancing a return stack is how one of them drifts. See guestcall.h. */
bool call_gear_applier(uint32_t actor, int part) {
    /* R[2] = 0 - the NULL transform, which 0x0200641C turns into the actor's
     * own. Two of the game's call sites already pass zero. */
    return lsw2_guest_call(lsw2_arm9_FUN_020063d4, kLsw2GearApplier,
                           actor, uint32_t(part), 0u);
}

}  // namespace

void lsw2_partapply_arm() { g_armed = true; }

void lsw2_partapply_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): a guest call that re-dresses an actor on one console is a divergence. */
    if (lsw2_vwh_lockstep()) return;
    /* A PROBE, but the same rule: from the tick the guest is usually halted and
     * a refused call would report a failure that never happened. Staying armed
     * costs a frame; reporting a false negative costs an investigation. */
    if (!lsw2_guest_ready()) return;
    if (!g_armed) return;
    g_armed = false;

    if (!ui_fork_build()) {
        std::fprintf(stderr, "[fork] part probe: fork build only\n");
        return;
    }

    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (actor < 0x02000000u || actor >= 0x02400000u) {
        std::fprintf(stderr,
                     "[fork] part probe: no party actor (%08X) - load a level\n",
                     actor);
        return;
    }

    const int part = int(read16(kLsw2CustomBase + kLsw2CustomSlotOffsets[0]));
    const int slot = lsw2_part_slot(part);
    if (slot < 0) {
        std::fprintf(stderr,
                     "[fork] part probe: customize part %d has no descriptor "
                     "(0xFA means the player chose nothing here)\n", part);
        return;
    }

    const uint32_t slot_addr = lsw2_actor_slot_addr(actor, slot);
    const uint16_t before = read16(slot_addr);

    const bool returned = call_gear_applier(actor, part);

    const uint16_t after = read16(slot_addr);

    std::fprintf(stderr,
                 "[fork] part probe: applied part %d (slot %d) to actor %08X\n"
                 "   returned cleanly : %s\n"
                 "   slot before      : %d\n"
                 "   slot after       : %d  %s\n",
                 part, slot, actor,
                 returned ? "yes" : "NO - the call did not come back",
                 int(before), int(after),
                 after == uint16_t(part) ? "<- the part took"
                                         : "<- unchanged, the call did nothing");
}
