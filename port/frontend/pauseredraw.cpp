/* See pauseredraw.h. Ask the game to redraw its own menu. */

#include "pauseredraw.h"

#include "gearhop.h"

#include <cstdio>

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

int g_reports = 0;
constexpr int kReports = 4;

}  // namespace

void lsw2_pause_request_redraw(uint32_t menu) {
    if (menu < kRamLow || menu >= kRamHigh) return;

    /* ON THE GUEST'S THREAD, AT THE TOP OF THE PAUSE MENU'S NEXT UPDATE - see
     * pauseredraw.h. A repeat of a request still waiting is refused by the hop
     * as a duplicate, which is what "only the most recent request is kept"
     * always meant: there is one pause menu. */
    const bool queued = lsw2_gearhop_request_action_at(
        kLsw2PauseRebuild, menu, kLsw2PauseRebuildCursorMoved, 0u, 0u,
        kLsw2PauseStateUpdate);
    if (g_reports < kReports) {
        ++g_reports;
        std::fprintf(stderr, "[fork] pause rebuild %08X %s for the pause "
                             "menu's next update\n",
                     unsigned(menu), queued ? "queued" : "already waiting");
    }
}

bool lsw2_pause_rebuild_still_live(uint32_t menu, uint32_t update_object) {
    if (update_object == menu) return true;
    std::fprintf(stderr, "[fork] pause rebuild %08X dropped - the pause menu "
                         "updating now is %08X, so the menu it was for is gone\n",
                 unsigned(menu), unsigned(update_object));
    return false;
}
