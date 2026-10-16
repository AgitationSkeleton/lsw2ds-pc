/* See menuhost.h. */

#include "menuhost.h"

#include "gamemenu.h"

#include <cstdio>

namespace {

Lsw2Menu* g_menu = nullptr;
uint16_t g_previous = 0;
Lsw2MenuEvent g_last = Lsw2MenuEvent::None;

/* KEYS STILL DOWN FROM THE MENU THAT JUST CLOSED, withheld from the guest until
 * each is let go. The guest saw nothing while the menu was open, so a key held
 * across the close would reach it as a FRESH press - measured: the A that
 * chose Host Settings' Character row closed
 * the menu, reached the game as a new A while the host stood at the lobby
 * console, and the console's script answered it - reopening the console over
 * the Free Play grid the row had just opened. */
uint16_t g_latched = 0;

}  // namespace

Lsw2Menu* lsw2_menuhost_current() { return g_menu; }

void lsw2_menuhost_open(Lsw2Menu* menu) {
    g_menu = menu;
    /* CLEAR THE EDGE STATE ON OPEN. Without this, the A press that opened the
     * menu is still in the held mask on the first frame, and the menu sees it
     * as a fresh press - activating whatever row the cursor happens to start
     * on. The menu would appear and immediately act on its own opening. */
    g_previous = 0xFFFFu;
    g_last = Lsw2MenuEvent::None;
}

void lsw2_menuhost_close() {
    /* SAY SO, ONCE PER CLOSE, WHILE THE FLICKER IS BEING CHASED.
     *
     * THE SYMPTOM: the console menu appears and vanishes. Opening is
     * now edge-triggered (see lsw2_fork_console_interact), which fixes the
     * re-open half. The CLOSING half has no witness at all - g_menu is set to
     * null here from four different places and the backdrop's "restored" line
     * is the only sign any of them ran, which cannot say which.
     *
     * A close is rare - a menu a player is using closes once - so this costs
     * nothing and is the difference between watching the symptom and seeing
     * the cause. */
    if (g_menu) std::fprintf(stderr, "[fork] menu closed\n");
    if (g_menu) g_latched = g_previous;   /* what the menu last saw held */
    g_menu = nullptr;
}


bool lsw2_menuhost_feed_keys(uint16_t held) {
    if (!g_menu) {
        g_previous = held;
        return false;
    }
    const uint16_t pressed = uint16_t(held & ~g_previous);
    g_previous = held;

    g_last = g_menu->step(pressed);
    if (g_last == Lsw2MenuEvent::Cancelled) lsw2_menuhost_close();
    return true;
}

bool lsw2_menuhost_open_now() { return g_menu != nullptr; }

void lsw2_menuhost_touch_row(int row) {
    if (!g_menu) return;
    const int before = g_menu->cursor();
    g_menu->set_cursor(row);
    if (g_menu->cursor() != row) return;   /* hidden or out of range */

    /* A TAP ON A ROW THE CURSOR IS NOT ON MOVES THERE FIRST, and does not also
     * activate. That is how the cartridge's own touch menus behave and it is
     * what stops a mis-tap committing a setting: the first tap selects, the
     * second acts. A tap on the row already under the cursor activates it. */
    if (before != row) return;   /* selected only; the next tap acts */

    /* ACTIVATION GOES THROUGH step() WITH THE A BIT, not through a second
     * path that happens to return Activated. A row that one day needs setting
     * up before it activates would otherwise work from the d-pad and not from
     * a tap, and nothing would say so. */
    g_last = g_menu->step(kLsw2KeyA);
    if (g_last == Lsw2MenuEvent::Cancelled) lsw2_menuhost_close();
}

Lsw2MenuEvent lsw2_menuhost_take_event() {
    /* TAKEN, so one press produces one action. Leaving it set would re-fire
     * the same Activated on every frame the key stayed down. */
    const Lsw2MenuEvent e = g_last;
    g_last = Lsw2MenuEvent::None;
    return e;
}

uint16_t lsw2_menuhost_guest_keys(uint16_t held) {
    /* THE GUEST SEES NOTHING while a menu is open.
     *
     * Not "nothing but the d-pad" or "nothing but A" - nothing. The fork's
     * menus sit on top of a live lobby where the player has a character, and
     * any key that reaches the guest moves it. Letting one class through would
     * mean deciding which, and there is no key the game does not use. */
    if (g_menu) return 0u;
    g_latched = uint16_t(g_latched & held);   /* a key let go is the game's again */
    return uint16_t(held & ~g_latched);
}
