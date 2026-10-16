/* See lobbyhud.h. */

#include "lobbyhud.h"

#include "forksession.h"
#include "freeplaypick.h"
#include "gamefont.h"
#include "gamemenu_render.h"
#include "topoverlay.h"
#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* WHERE THE CARTRIDGE PUTS ITS COUNTDOWN. The statue select draws its
 * own timer at (0x80, 0x48) - the widget call is
 * `0x02065BE8(widget, 1, 0x80, 0x48, text, 1)`. Matching it means the two
 * countdowns in this game appear in the same place, which is what makes this
 * read as the game's rather than as the port's. */
constexpr int kCountdownY = 0x48;

/* The same white the menu rows use. One palette, one place. */
constexpr uint32_t kCountdownColour = 0xFFFFFFu;

/* THE ALL PICK'S COUNTDOWN IS THE SELECT'S RED - sampled off the statue
 * select's own digits in a capture (conn_select41: 0xF32828 at the face,
 * 0xB22020 in the shade), with its title a line above. */
constexpr uint32_t kPickCountColour = 0xF32828u;
constexpr int kPickTitleGap = 18;

/* The game's seconds counter, the clock every timer in the fork now
 * shares - the statue select's 30, the score screen's 15, and this. */
/* See lobbyhud.h. */

/* Draw one line into the top-screen layer and say which rows it took. */
void line(uint32_t* layer, int width, int y, const char* text, uint32_t rgb) {
    lsw2_menu_draw_centred(layer, width, y, text, rgb);
    lsw2_top_overlay_touched(y, y + lsw2_menu_text_rows(text));
}

}  // namespace

void lsw2_lobby_countdown_composite(uint32_t* layer, int width) {
    if (!layer || width <= 0 || !ui_fork_build() || !lsw2_font_ready()) return;

    /* ANY WIDTH: `lsw2_menu_draw_centred` centres on the frame's real width,
     * so a widened frame still has the number in the middle. */

    /* THE FREE PLAY PICKERS' WORDS FIRST (freeplaypick.h): while one is up the
     * touch screen is the grid, so what the select would have said goes here.
     * The ALL pick shows the select's own title over the select's own
     * countdown; the host's picker says what a tap does. */
    const int pick_left = lsw2_freeplaypick_pregame_seconds_left();
    const bool host_pick = lsw2_freeplaypick_host_active();

    const uint32_t now = bus_device_read32(0, kLsw2SecondsClock);
    const int left = lsw2_fork_session().lobby_remaining(now);
    if (left < 0 && pick_left < 0 && !host_pick) return;   /* nothing to show */

    char text[8];
    if (pick_left >= 0) {
        line(layer, width, kCountdownY - kPickTitleGap, "Select Character",
             kCountdownColour);
        std::snprintf(text, sizeof text, "%d", pick_left);
        line(layer, width, kCountdownY, text, kPickCountColour);
        return;
    }
    if (host_pick) {
        line(layer, width, kCountdownY - kPickTitleGap,
             "Choose the One character", kCountdownColour);
        line(layer, width, kCountdownY, "Double-tap to pick, B to go back",
             kCountdownColour);
        return;
    }

    /* JUST THE NUMBER. String `0x296` is `"@1"` - a lone parameter placeholder -
     * so the cartridge's countdown has no label, and neither does this. */
    std::snprintf(text, sizeof text, "%d", left);
    line(layer, width, kCountdownY, text, kCountdownColour);
}
