/* See freezewatch.h. */

#include "freezewatch.h"

#include "cheats.h"
#include "state.h"
#include "ui.h"

#include <cstdio>

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;
constexpr int kBytes = int(kLsw2FreezeWatchLast - kLsw2FreezeWatchFirst);

/* A SECOND, because the freeze is described as lasting until the player acts -
 * so anything still different a second later is not a transient of the
 * interaction itself. */
constexpr int kSettleFrames = 60;

uint32_t g_actor = 0;
uint8_t g_before[kBytes];
int g_countdown = -1;
bool g_said = false;

uint8_t read8(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

}  // namespace

void lsw2_freezewatch_arm() {
    if (g_said) return;                 /* one report is the whole point */
    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (actor < kRamLow || actor >= kRamHigh) return;

    g_actor = actor;
    for (int i = 0; i < kBytes; ++i)
        g_before[i] = read8(actor + kLsw2FreezeWatchFirst + uint32_t(i));
    g_countdown = kSettleFrames;
}

void lsw2_freezewatch_tick() {
    if (g_countdown < 0 || g_said) return;
    if (--g_countdown > 0) return;

    g_countdown = -1;
    if (g_actor < kRamLow || g_actor >= kRamHigh) return;

    /* THE ACTOR MUST STILL BE THE SAME ONE. A character switch between arming
     * and reporting would diff two different people and name every byte. */
    if (lsw2_cheats_party_actor(0) != g_actor) {
        std::fprintf(stderr, "[fork] freeze watch: the actor changed - no "
                             "diff to report\n");
        g_said = true;
        return;
    }

    g_said = true;
    int changed = 0;
    std::fprintf(stderr, "[fork] freeze watch on actor %08X, one second after "
                         "the console:\n", unsigned(g_actor));
    for (int i = 0; i < kBytes; ++i) {
        const uint32_t off = kLsw2FreezeWatchFirst + uint32_t(i);
        const uint8_t now = read8(g_actor + off);
        if (now == g_before[i]) continue;
        ++changed;
        std::fprintf(stderr, "       +0x%03X  %02X -> %02X\n",
                     unsigned(off), unsigned(g_before[i]), unsigned(now));
    }
    if (changed == 0) {
        /* WORTH SAYING, because it is a real answer and not a failure: the
         * lock would then be outside this window, or not on the actor at all,
         * and the next search should start somewhere else rather than looking
         * harder here. */
        std::fprintf(stderr, "       nothing in 0x%03X..0x%03X changed - the "
                             "lock is elsewhere\n",
                     unsigned(kLsw2FreezeWatchFirst),
                     unsigned(kLsw2FreezeWatchLast));
    }
}
