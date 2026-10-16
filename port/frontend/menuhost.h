/* Which fork menu is open, if any.
 *
 * One place owns "is a menu up", because two things need to agree about it and
 * they live far apart:
 *
 *   - the PRESENT path composites the menu into the top screen
 *   - the INPUT path must stop the guest seeing the keys the menu is using
 *
 * If those disagree, the player drives their character around the lobby while
 * reading a settings menu, which is the kind of bug that is obvious on screen
 * and invisible in code.
 */

#ifndef LSW2_MENUHOST_H
#define LSW2_MENUHOST_H

#include <cstdint>

#include "gamemenu.h"

class Lsw2Menu;

/* The menu currently open, or null. */
Lsw2Menu* lsw2_menuhost_current();

/* Open one of the fork's menus. Passing null closes whatever is open. */
void lsw2_menuhost_open(Lsw2Menu* menu);
void lsw2_menuhost_close();

/* THERE IS NO `lsw2_menuhost_open_p()`, and there was.
 *
 * It returned `g_menu != nullptr` - the same question `lsw2_menuhost_current()`
 * already answers, since callers test the pointer anyway - and nothing ever
 * called it. Two ways to ask one question, one of them unused, is a fork waiting
 * for the two to disagree the moment "open" stops meaning "has a menu".
 *
 * Found by the fourth run of the declared-but-never-called sweep, which also
 * caught a function of my own the pass before. */

/* Feed this frame's DS key mask, active HIGH (a set bit means pressed).
 *
 * Edge detection happens here rather than in the caller: the menu wants
 * newly-pressed keys, the frontend has a held mask, and putting the previous
 * mask in one place stops two callers keeping two copies that drift.
 *
 * Returns true when the menu consumed the input, which is the caller's signal
 * to hide it from the guest. */
bool lsw2_menuhost_feed_keys(uint16_t held);

/* Is a menu holding the guest's input right now?
 *
 * Added for the console freeze (the player stuck until they jump), which has
 * two explanations that need
 * opposite fixes: an invisible menu swallowing every key, or the guest's own
 * interaction state left locked. `lsw2_menuhost_guest_keys` returns 0 while a
 * menu is up, so the player could not jump either - and they can. This is how
 * the next interaction says which. */
bool lsw2_menuhost_open_now();

/* A TAP LANDED ON `row`. Moves the cursor there and activates it, producing
 * the same event a press of A would.
 *
 * THE BUG THIS FIXES: the touch screen did not drive the menus, it drove the
 * widgets underneath. The menus are on the touch screen now and this is what
 * makes them answer to it. Deliberately the SAME event path as the d-pad, so a
 * row cannot behave one way when pressed and another when tapped. */
void lsw2_menuhost_touch_row(int row);

/* The event the last key feed produced, cleared by reading it.
 *
 * IT WAS STORED AND NEVER READ. `lsw2_menuhost_feed_keys` has written
 * `g_last` since it was written, and nothing anywhere took it - so every
 * Activated and ValueChanged the player produced was dropped on the floor and
 * `Lsw2ForkSession::on_menu_event`, which acts on them, was called only from
 * `test_forksession.cpp`. The fork's menus moved their cursor and did nothing
 * else: the arena menu, host settings, the console's six options, the kick
 * list, the master browser and the join screen, all of the online lobby's
 * in-game UI.
 *
 * That is why the console appeared to freeze the player until they jumped.
 * A menu opened, `lsw2_menuhost_guest_keys` ate every key, and only
 * B did anything, because Cancelled is the one event the host handled itself.
 *
 * Found by `tools/sweep_log_reachable.py`, which asks whether the function
 * containing a diagnostic can be reached at all. */
Lsw2MenuEvent lsw2_menuhost_take_event();

/* What the guest should see this frame. Zero while a menu is open, so the
 * player's character does not walk around behind it. */
uint16_t lsw2_menuhost_guest_keys(uint16_t held);

#endif /* LSW2_MENUHOST_H */
