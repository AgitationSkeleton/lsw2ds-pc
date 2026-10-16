/* See hatstay.h. */

#include "hatstay.h"

#include "cheats.h"     /* kLsw2ActorCharacter */
#include "partslot.h"   /* lsw2_actor_slot_addr */
#include "ui.h"

#include "state.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>

extern "C" void lsw2_arm9_FUN_0205e80c(void);

namespace {

constexpr int kHeadwearSlot = 3;

int g_kept = 0;
constexpr int kKeptReports = 8;

uint16_t rd16(uint32_t addr) {
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    return uint16_t((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

uint8_t rd8(uint32_t addr) {
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

}  // namespace

void lsw2_hatstay_spawn_override() {
    /* A KILL-SWITCH FOR THE CONTROL RUN, not a setting (the LSW2_HUDPORTRAIT_OFF
     * shape): with it set, the same scripted hit shows what the game does
     * without this fix, on the same build. */
    static const bool off = std::getenv("LSW2_HATSTAY_OFF") != nullptr;
    if (g_cpu.R[14] == kLsw2KnockoffSpawnReturn && ui_fork_build() && !off) {
        /* THE HIT HANDLER'S KNOCK-OFF, and the hat stays on. The handler keeps
         * its actor in r4 throughout (020052E8 reads the character through it),
         * and nothing between this call and 020053EC moves sp, so the epilogue
         * unwinds exactly the frame the handler built. */
        const uint32_t actor = g_cpu.R[4];
        ++g_kept;
        if (g_kept <= kKeptReports) {
            std::fprintf(stderr,
                         "[fork] hat stays on: the hit on actor %08X (character "
                         "%u) would have knocked off headwear %u - resuming the "
                         "hit handler at its epilogue instead (%d so far)\n",
                         unsigned(actor),
                         unsigned(rd8(actor + kLsw2ActorCharacter)),
                         unsigned(rd16(lsw2_actor_slot_addr(actor, kHeadwearSlot))),
                         g_kept);
        }
        g_cpu.R[15] = kLsw2HitHandlerEpilogue;
        return;
    }

    /* EVERY OTHER SPAWN: the game's own function, unchanged. R[15] names the
     * entry because the lifted function picks its landing pad from it; R[14]
     * is left alone so it returns to its caller. */
    g_cpu.R[15] = kLsw2Spawner;
    lsw2_arm9_FUN_0205e80c();
}
