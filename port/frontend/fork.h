/* The Bounty Hunter fork's own behaviour.
 *
 * Everything here is gated on ui_fork_build() and does nothing in the baseline,
 * which is kept vanilla-esque by design. Kept in its own file rather than
 * scattered through frontend.cpp so that "what does the fork do differently" is
 * answerable by reading one thing.
 */

#ifndef LSW2_FORK_H
#define LSW2_FORK_H

#include <cstddef>
#include <cstdint>

/* Called once per frame from the present loop, after the guest frame has been
 * stepped and before anything is presented.
 *
 * The fork's start: get the player into the Cantina Arena Lobby without them
 * seeing the boot sequence or the save select. */
void lsw2_fork_tick();

/* Whether the fork is still getting the player where they belong, and the top
 * screen should therefore NOT show the guest's own picture yet.
 *
 * The boot sequence is not defeated - it runs, exactly as the game wrote it,
 * because the warp needs the state machine it builds. What is suppressed is
 * only the SHOWING of it, which is a host-layer decision about presentation and
 * not a change to what the game computes. */
bool lsw2_fork_hiding_boot();

/* A JOINER BETWEEN THE JOIN AND ITS SEAT (while joining, the joiner's character
 * was observed to become the host's for a moment). The
 * cartridge's own join rebuilds the joiner's party from the session at once -
 * slot 0 is the HOST there - while the joiner's station byte still reads 0, so
 * its camera and HUD show the host's hunter as the player through the stock
 * join screens (34/39) and the loading screen (40), which are the touch
 * screen's, until the seat reload. The top screen is not shown for that
 * stretch, the way the boot is not: presentation only. */
bool lsw2_fork_hiding_join();

/* One line for the loading overlay, or nullptr once there is nothing to say.
 * Lets the shell explain a wait rather than showing a black rectangle. */
const char* lsw2_fork_boot_status();

/* THE FORK'S PAUSE MENU, per frame. The pause menu's hidden rows have to STAY hidden while
 * the player is scrolling, which the build-time fixups could not do - see
 * dispatch_overrides.cpp. Silent in the baseline and when no menu is open. */
void lsw2_pause_tick();

/* DS key bits the fork wants held this frame, in KEYINPUT bit order (A = bit 0,
 * Start = bit 3), or 0. Active HIGH - the caller inverts along with every other
 * input source.
 *
 * THE FORK'S START SKIPS THE BOOT SEQUENCE AND SAVE SELECT, and the honest
 * way to get past a menu is to press the button that gets past it. The warp
 * cannot fire until the game has built a level state, and the game does not
 * build one until the player has chosen a save slot - so something has to
 * answer those menus. Driving the game's own UI is the same principle as the
 * warp itself: use the path the game already takes.
 *
 * Returns 0 once the fork has arrived, so the player has the pad back. */
uint16_t lsw2_fork_boot_keys();

/* THE PLAYER TOUCHED THE ARENA LOBBY'S CONSOLE ENTITY.
 *
 * Called from the script-command seam, which is where that interaction becomes
 * visible: in the fork the console's own script refuses with "You must Save
 * before doing that!", and the fork waives that check - so the
 * refusal is intercepted and this is what happens instead.
 *
 * WHICH MENU DEPENDS ON THE SESSION, and both are the fork's own:
 *
 *   offline   the host settings menu - "Host Internet Game"'s destination,
 *             ten rows ending in "Start Lobby"
 *   hosting   the console entity's six options
 *
 * A client gets nothing, which is what Lsw2ForkSession::open_console already
 * decides: every option on that menu is an authority a client does not have. */
void lsw2_fork_console_interact();

/* A character's name, as the cartridge spells it.
 *
 * The host's character picker needs sixty-five of these, and the game already
 * holds them: names decode through string id `0x1CB + id`. Transcribing them into a table here would be sixty-five chances
 * to misspell one and a second source of truth to keep in step.
 *
 * Lives beside the string override because that is where the guest reader and
 * the control block already are. Writes into `buf` and returns it; on a decoder
 * that is not ready - the string bank is not always loaded - it writes a
 * truthful placeholder rather than an empty row. */
const char* lsw2_fork_character_name(int character_id, char* buf, size_t n);

#endif /* LSW2_FORK_H */
