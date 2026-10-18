/* See radarview.h. */

#include "radarview.h"

#include "cheats.h"        /* kLsw2PartyActors */
#include "enhanced.h"
#include "levels.h"        /* kLsw2LevelRootAddr */
#include "netcount.h"      /* kLsw2NetCountObject, kLsw2NetLocalStation */
#include "touchportrait.h" /* kLsw2TouchHudUpdate - the host */

#include "runtime_arm.h"
#include "state.h"

#include <cstdio>

namespace {

bool g_active = false;
uint32_t g_sp = 0, g_lr = 0;
uint32_t g_actor = 0;
uint16_t g_real = 0, g_fake = 0;
bool g_said = false;

uint16_t r16(uint32_t a) {
    return uint16_t(bus_device_read32(0, a & ~3u) >> ((a & 2u) * 8u));
}
void w16(uint32_t a, uint16_t v) {
    const uint32_t w = bus_device_read32(0, a & ~3u);
    const uint32_t shift = (a & 2u) * 8u;
    bus_device_write32(0, a & ~3u, (w & ~(0xFFFFu << shift)) | (uint32_t(v) << shift));
}

/* The real heading back - only over our own value. */
void restore() {
    if (r16(g_actor + kLsw2ActorHeading) == g_fake) w16(g_actor + kLsw2ActorHeading, g_real);
    g_active = false;
}

}  // namespace

bool lsw2_radarview_returned() {
    if (!g_active || g_cpu.R[13] != g_sp) return false;
    restore();
    g_cpu.R[15] = g_lr;
    return true;
}

void lsw2_radarview_enter() {
    /* An update that never came back (a level torn down under it) must not
     * leave the camera's heading on the actor. */
    if (g_active) restore();

    const uint32_t w = bus_device_read32(0, (kLsw2NetCountObject + kLsw2NetLocalStation) & ~3u);
    const uint32_t station = (w >> (((kLsw2NetCountObject + kLsw2NetLocalStation) & 3u) * 8u)) & 0xFFu;
    if (station >= 4u) return;
    const uint32_t actor =
        bus_device_read32(0, kLsw2LevelRootAddr + kLsw2PartyActors + station * 4u);
    uint16_t heading = 0;
    if (!lsw2_enhanced_orbit_heading(actor, &heading)) return;
    const uint16_t real = r16(actor + kLsw2ActorHeading);
    if (real == heading) return;

    g_actor = actor;
    g_real = real;
    g_fake = heading;
    g_sp = g_cpu.R[13];
    g_lr = g_cpu.R[14];
    w16(actor + kLsw2ActorHeading, heading);
    g_cpu.R[14] = kLsw2TouchHudUpdate;
    g_active = true;
    if (!g_said) {
        g_said = true;
        std::fprintf(stderr,
                     "[fork] radar: turning with the Enhanced camera (heading "
                     "%04X for the touch HUD, the player's is %04X)\n",
                     unsigned(heading), unsigned(real));
    }
}
