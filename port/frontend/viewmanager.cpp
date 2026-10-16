/* See viewmanager.h. */

#include "viewmanager.h"

#include "netcount.h"
#include "viewmask.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

uint32_t read32(uint32_t a) { return bus_device_read32(0, a); }

}  // namespace

Lsw2ViewManagerState lsw2_viewmanager_state() {
    Lsw2ViewManagerState s;
    s.manager = read32(kLsw2ViewManagerPtr);
    s.pump_gate = read32(kLsw2PumpCountWord);
    s.players = lsw2_netcount_guest();
    return s;
}

void lsw2_viewmanager_report() {
    if (!ui_fork_build()) return;

    /* SAID ON CHANGE, not every frame. Three numbers at sixty hertz is noise;
     * the transitions are the whole signal, and there are very few of them. */
    static uint32_t last_manager = 1;  /* an impossible first value */
    static uint32_t last_gate = 1;
    static int last_players = -1;

    const Lsw2ViewManagerState s = lsw2_viewmanager_state();
    if (s.manager == last_manager && s.pump_gate == last_gate &&
        s.players == last_players)
        return;
    last_manager = s.manager;
    last_gate = s.pump_gate;
    last_players = s.players;

    std::fprintf(stderr,
                 "[fork] netgame bookkeeping: view manager %08X, pump gate "
                 "%08X, players %d%s\n",
                 unsigned(s.manager), unsigned(s.pump_gate), s.players,
                 s.manager ? "" : "  (null - no cartridge netgame state; "
                                  "see viewmanager.h)");
}
