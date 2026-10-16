/* See menubackdrop.h. */

#include "menubackdrop.h"

#include "gpu2d.h"
#include "menuhost.h"
#include "screenengine.h"
#include "ui.h"

#include <cstdio>

namespace {

bool g_hidden = false;
unsigned g_saved_dispcnt = 0;
/* The register actually poked, so the restore reaches the same engine even if
 * POWCNT1's display swap moves while a menu is open - screenengine.h. */
uint32_t g_poked = 0;

}  // namespace

void lsw2_menubackdrop_tick() {
    if (!ui_fork_build()) return;

    const bool want = lsw2_menuhost_current() != nullptr;

    if (want) {
        const uint32_t reg = lsw2_screen_dispcnt(kLsw2ScreenTouch);
        const unsigned now = unsigned(nds_gpu2d_read(reg, 4));

        if (!g_hidden) {
            g_saved_dispcnt = now;
            g_poked = reg;
            g_hidden = true;
            std::fprintf(stderr,
                         "[fork] menu backdrop: touch screen down to the "
                         "starfield (DISPCNT %08X was %08X)\n",
                         unsigned(reg), now);
        }

        /* WHAT THE GAME WANTS, ACCUMULATED - not a snapshot taken once.
         *
         * THE BUG. The player leaves the lobby console and the
         * Free Play icons never come back - the touch screen is left as bare
         * starfield. Measured either side of it: engine B's OAM is UNCHANGED,
         * byte for byte, so the icon sprites are still there; engine B's DISPCNT
         * went from 0x00011F50 to 0x00010450, which is OBJ off and BG0, BG1 and
         * BG3 off, leaving only BG2 - the starfield. Nothing was lost but the
         * permission to draw it.
         *
         * WHY THE RESTORE BELOW RESTORED NOTHING. `g_saved_dispcnt` was taken on
         * the single frame the menu opened, and the log says what it caught:
         *
         *     menu backdrop: touch screen down to the starfield
         *                    (DISPCNT 04001000 was 40010453)
         *
         * 0x...0453 is BG2 only with OBJ already off - the grid had not been
         * drawn yet. So the snapshot held none of the bits that matter, the
         * clear below then suppressed every layer the game brought up WHILE the
         * menu was open, and on close `saved & mask` was zero and the restore
         * was a no-op. The grid never came back.
         *
         * That is exactly why the report says the console "CAN bug" it: open the
         * console after the grid is up and the snapshot is good and the restore
         * works; open it before, and the grid is lost for the rest of the visit.
         *
         * SO REMEMBER EVERY LAYER THE GAME HAS ASKED FOR, not just the ones it
         * had asked for on one arbitrary frame. The guest rewrites this register
         * every frame, so `now` carries its intent before the clear.
         *
         * OR, NEVER ASSIGN, and that is deliberate. If a frame ever arrives
         * where the guest did not rewrite the register, `now` is the value this
         * function wrote last frame - all the masked bits clear - and assigning
         * would erase the memory and reintroduce the bug. Accumulating can only
         * over-restore a layer the game turned off on purpose while the menu was
         * up, which the guest's own next write corrects within a frame; the
         * alternative loses the whole screen until the player leaves. */
        g_saved_dispcnt |= (now & kLsw2MenuHiddenLayers);

        /* KEEP THE STARFIELD, DROP THE REST. Re-asserted every frame because
         * the guest rewrites this register - arenahud.cpp's note. */
        const unsigned wanted = now & ~kLsw2MenuHiddenLayers;
        if (now != wanted) nds_gpu2d_write(g_poked, wanted, 4);
        return;
    }

    if (g_hidden) {
        /* PUT BACK ONLY THE BITS WE TOOK. The game will have changed other
         * things about its own display while the menu was up. */
        const unsigned now = unsigned(nds_gpu2d_read(g_poked, 4));
        const unsigned put_back = g_saved_dispcnt & kLsw2MenuHiddenLayers;
        nds_gpu2d_write(g_poked, now | put_back, 4);
        g_hidden = false;
        /* SAY WHICH LAYERS CAME BACK. "restored" on its own was true of the
         * run that lost the whole Free Play grid - it restored nothing, and
         * said so in exactly the same words as a restore that worked. */
        std::fprintf(stderr,
                     "[fork] menu backdrop: touch screen restored "
                     "(DISPCNT %08X -> %08X, put back %04X)\n",
                     now, now | put_back, put_back);
    }
}
