/* See killcredit.h. */

#include "killcredit.h"

#include "cheats.h"        /* kLsw2PartyActors, kLsw2ActorViewIndex */
#include "forksession.h"
#include "levels.h"        /* kLsw2LevelRootAddr */
#include "ui.h"

#include "runtime_arm.h"
#include "state.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_02004dec(void);

namespace {

/* The killer object's fields the handler reads (killcredit.h). */
constexpr uint32_t kObjectType = 0x0Cu;           /* u16 */
constexpr uint16_t kTypeProjectile = 0x10u;
constexpr uint16_t kTypeExplosive = 0x8000u;
constexpr uint32_t kObjectOwner = 0x1BCu;         /* s32 station */
constexpr int kNoView = 4;

int g_said = 0;

bool ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }
uint32_t r32(uint32_t a) { return bus_device_read32(0, a); }
uint8_t r8(uint32_t a) { return uint8_t(r32(a & ~3u) >> ((a & 3u) * 8u)); }
uint16_t r16(uint32_t a) { return uint16_t(r32(a & ~3u) >> ((a & 2u) * 8u)); }

/* The station the handler would credit with this kill, or -1 - its own two
 * rules, in its own order. */
int credited_station(uint32_t killer) {
    for (uint32_t i = 0; i < 4; ++i)
        if (r32(kLsw2LevelRootAddr + kLsw2PartyActors + i * 4u) == killer) return int(i);
    const uint16_t type = r16(killer + kObjectType);
    if (type != kTypeProjectile && type != kTypeExplosive) return -1;
    const int32_t owner = int32_t(r32(killer + kObjectOwner));
    return owner < 4 ? int(uint32_t(owner) & 0xFFu) : -1;
}

}  // namespace

void lsw2_killcredit_death() {
    const uint32_t victim = g_cpu.R[0];
    const uint32_t killer = g_cpu.R[1];
    if (ui_fork_build() && lsw2_fork_session().settings().suicide_no_kill &&
        ram(victim) && ram(killer)) {
        const int station = r8(victim + kLsw2ActorViewIndex);
        if (station != kNoView && credited_station(killer) == station) {
            g_cpu.R[1] = 0;
            if (g_said < 8) {
                ++g_said;
                std::fprintf(stderr,
                             "[fork] kill credit: station %d caused its own death "
                             "(killer %08X) - scored as a death with no killer\n",
                             station, unsigned(killer));
            }
        }
    }
    g_cpu.R[15] = kLsw2DeathHandler;
    lsw2_arm9_FUN_02004dec();
}
