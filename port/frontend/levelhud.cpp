/* See levelhud.h. */

#include "levelhud.h"

#include "gearhop.h"
#include "hudportrait.h"   /* kLsw2HudStep - the level state's update */
#include "statelog.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* Game Settings, the map/gamemode/time chooser (diagnostics.cpp). */
constexpr int kGameSettings = 0x36;

/* The level state whose HUD the fork hid, or 0. */
uint32_t g_hidden = 0;

uint32_t world_state() {
    return bus_device_read32(0, kLsw2StateManager + kLsw2SlotResult +
                                    kLsw2WorldSlot * 4u);
}

bool is_level(uint32_t state) {
    return state >= 0x02000000u && state < 0x02400000u &&
           bus_device_read32(0, state + kLsw2StateKind) == kLsw2LevelStateKind;
}

}  // namespace

bool lsw2_levelhud_message_still_live(uint32_t state, uint32_t updating) {
    return state == updating && is_level(state);
}

void lsw2_levelhud_tick() {
    if (!ui_fork_build()) return;

    const bool settings = lsw2_state_current() == kGameSettings ||
                          lsw2_state_pending() == kGameSettings;
    const uint32_t world = world_state();

    if (settings && !g_hidden && is_level(world)) {
        if (lsw2_gearhop_request_action_at(kLsw2LevelStateMessage, world,
                                           kLsw2LevelHudHide, 0u, 0u,
                                           kLsw2HudStep)) {
            g_hidden = world;
            std::fprintf(stderr,
                         "[fork] Game Settings over the lobby: the level's HUD "
                         "is asked to step aside (message 0x12 to %08X) so its "
                         "hearts stay out of the map preview\n",
                         unsigned(world));
        }
        return;
    }

    if (!settings && g_hidden) {
        /* ONLY TO THE SAME LEVEL, STILL ALIVE. After a round start the lobby
         * was rebuilt and its new HUD was never hidden. */
        if (world == g_hidden && is_level(world) &&
            lsw2_gearhop_request_action_at(kLsw2LevelStateMessage, world,
                                           kLsw2LevelHudShow, 0u, 0u,
                                           kLsw2HudStep)) {
            std::fprintf(stderr,
                         "[fork] Game Settings closed: the lobby's HUD is given "
                         "the top screen back (message 0x13 to %08X)\n",
                         unsigned(world));
        }
        g_hidden = 0;
    }
}
