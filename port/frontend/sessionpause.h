/* THE ONLINE PAUSE IS ONE PLAYER'S, AND IT FREEZES NOTHING.
 *
 * On an online connection the pause menu is not shared between consoles, and
 * opening it does not freeze gameplay.
 *
 * WHERE THERE IS A PAUSE AT ALL. FUN_02093bd8 answers Start only while the
 * screen slot holds state 48 (the lobby's touch screen) or 53 (the menu
 * itself). A match's screen is 42, so a match has no Start pause; the one that
 * froze online play is the lobby's.
 *
 * WHAT THE CARTRIDGE DOES. Its multi-card pause is one pause for everyone
 * Start with the multi-view bit up sends message 0x2B, and every
 * console's arm runs FUN_0208ce8c: FUN_02093c9c(_, 1) - the pause bits of
 * [0x027E02A8], each slot's pause hook 0x0200FAE8 and the sound system's pause
 * (0x020B9248/918C/9088) - then state 53 in the screen slot. Every console's
 * world stops (the world update FUN_0208cdb0 returns first while those bits
 * are set) until someone's Resume (0x2C) closes every menu.
 *
 * WHAT IS CHANGED, and only in a lockstep session (every choice below depends
 * only on the dataset, so every console makes it at the same step):
 *
 *  1. WHOSE MENU. The pause messages are dispatched through
 *     sessionpause_policy.h: the sender's console opens state 53 itself - the
 *     same request FUN_0208ce8c makes, WITHOUT FUN_02093c9c - and every other
 *     console drops the message. Resume and a client's Quit are dropped
 *     elsewhere too. Nothing is frozen anywhere, and the music plays on.
 *
 *  2. WHOSE INPUT. Every console marks the paused station and keeps its input
 *     away from the world: the sampler (FUN_0205ce64, first in the manager's
 *     frame 0x0205ED08) fills each station's word at 0x021027F8 + i*10 from
 *     that station's view record (+0x64), and this zeroes a paused station's
 *     word twice - in the pump's override, before the state updates, and in
 *     0x020B8E40's, between the updates and the entity pass 0x0205F520. So
 *     the player's character stands still on every console, identically,
 *     while the world around it runs. The paused console's OWN menu still
 *     needs the keys: its update (0x0208E514, reached only through the
 *     vtable) gets the word back for the length of its own call, and the
 *     second zeroing takes it away again before any entity reads it.
 *
 *  3. WHEN IT ENDS WITHOUT A RESUME. A round start, a match end or a lobby
 *     return replaces the menu on the paused console as it replaces the
 *     screen everywhere else. The mask is only changed by messages, so that
 *     console then sends the game's own 0x2C itself (through the gear hop),
 *     which unmarks it everywhere at one dataset - and, its menu being gone,
 *     is dropped on its own console too (the policy's Resume rule).
 *
 * EDGES. The menu's word, and the first word after a pause, get the edges the
 * sampler would have made against the station's last REAL keys
 * (sessionpause_policy.h). Against the zeroed word every held key would be a
 * fresh press each frame - measured, one Down raced the cursor round the menu -
 * and the A that chose Resume would be a jump. A station that leaves the
 * session is unmarked with the departure. */

#ifndef LSW2_SESSIONPAUSE_H
#define LSW2_SESSIONPAUSE_H

#include <cstdint>

/* The pump's override, before the pump runs: zero every paused station's
 * input word (after keeping the real one), and give a station just unpaused
 * its true edges. */
void lsw2_sessionpause_after_sampler();

/* The message dispatcher's override: a pause-menu message in a lockstep
 * session. True when it was handled here (dropped, or this console's menu
 * opened); false to run the cartridge's arm. */
bool lsw2_sessionpause_message(uint32_t msg, int station);

/* The pause state's update (0x0208E514), before it runs: this console's own
 * input word, back for the menu. */
void lsw2_sessionpause_menu_update();

/* 0x020B8E40, between the state updates and the entity pass: zero again. */
void lsw2_sessionpause_before_entities();

/* The dataset's members: a station that left is no longer paused. */
void lsw2_sessionpause_members(uint16_t members);

/* The fork tick: this console's menu was closed by a transition while it was
 * still marked paused - send the game's own Resume so every console agrees. */
void lsw2_sessionpause_tick();

/* Whether this console's own pause menu is up (for the arena HUD, which would
 * otherwise hide the touch screen the menu is on). */
bool lsw2_sessionpause_local_menu_up();

#endif /* LSW2_SESSIONPAUSE_H */
