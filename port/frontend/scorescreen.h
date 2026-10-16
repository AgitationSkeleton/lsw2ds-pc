/* The online match flow's fifteen-second score screen.
 *
 * At the end of a match the score screen stays up on a 15 s timer RATHER THAN
 * WAITING FOR SOMEONE TO RETURN TO THE LOBBY BY HAND; after it, everyone goes
 * back to the lobby, and an enabled lobby timer restarts.
 *
 * THE "INSTEAD OF" IS A STATEMENT ABOUT THE CARTRIDGE, and reading it as
 * decoration cost a pass: there is no fifteen-second constant in the ROM to
 * find. The screen waits for a button, forever, and the port has to add the
 * timer.
 *
 * WHAT THE SCREEN IS:
 *
 *   state       0x2C, the id the netplay dispatcher compares
 *   selection   [state + 0x8C], a byte; the enter sets it to 0
 *   options     0x2A5 + row -> "Continue", "Select Character", "Settings", "Quit"
 *   commit      0x0209CB94(state) dispatches on that byte alone
 *
 * SO THE PORT DOES NOT NEED TO CALL ANYTHING. It writes the byte the game is
 * about to read and presses A, which is the same path a player takes. Driving
 * the input rather than the function is what keeps this the game's own flow:
 * every side effect - the message, the seed, the state request - is the
 * cartridge's, in the cartridge's order.
 *
 * ROW 0, "Continue", IS NOT THE LOBBY RETURN DESCRIBED ABOVE - MEASURED WRONG.
 * This used to say that Quit leaves, that the two middle rows start a
 * differently-configured round, and that only Continue matches the specified
 * return of everyone to the lobby with a restarted lobby timer.
 *
 * The eliminations were all correct and the conclusion was not. Pressing
 * Continue produces `state 42, level 5` - another match on the same map,
 * immediately. Row 0 sends message 0x0D and means *play again*.
 *
 * NO ROW RETURNS TO A CANTINA LOBBY, because the retail wireless arena has no
 * walkaround lobby between rounds - players sit on a starfield, which is the
 * exact thing the fork's lobby replaces. So the lobby return is the FORK'S to build,
 * like the lobby itself, and elimination could never have found it: the right
 * option was not on the list.
 *
 * Continue is still the row this file presses, because dismissing the screen at
 * fifteen seconds is correct and measured. What follows the dismissal is the
 * open work.
 *
 * THE CLOCK IS THE GAME'S. `[0x02109258]` is the seconds counter the statue
 * select's own 30-second timer runs on, so this timer and the statue select's
 * usual 30 s timer are measured the same way. Counting the
 * port's frames instead would drift against everything else on screen.
 */

#ifndef LSW2_SCORESCREEN_H
#define LSW2_SCORESCREEN_H

#include <cstdint>

/* The specified duration. Named rather than inlined because it is a design
 * choice, not an implementation detail. */
constexpr int kLsw2ScoreSeconds = 15;

/* Once a frame, from the fork's tick. Cheap and silent outside state 0x2C. */
void lsw2_score_tick();

/* THE LOBBY RETURN IN A SHARED SESSION. The host's timer stamps it on
 * a dataset (vwh.h, kLsw2EffectLobbyReturn), and this runs on every console when
 * that dataset is consumed: it asks for the session lobby the way the game's own
 * lobby reload does - level 9 CoopEnter in the world slot, state 48 in the
 * screen slot, in one step - so the writes land at the same step everywhere
 * instead of at four consoles' separate ticks. */
void lsw2_score_lobby_return_begin();

/* The party's hearts refilled and the pair lsw2_score_lobby_return_begin asks
 * for - also for a return that is not the round's: a client whose host vanished
 * mid-match (fork.cpp). */
void lsw2_lobby_return_pair();

/* Keys the fork wants held this frame, in KEYINPUT bit order, ORed into the
 * guest's mask by the present loop.
 *
 * A SEPARATE CHANNEL FROM THE DEBUG SURFACE'S `g_keys_injected`, deliberately.
 * That one belongs to pilot.py; sharing it would mean a debug session and this
 * timer could clear each other's bits, and a shipped feature that fights a tool
 * is a bug nobody can reproduce. */
uint16_t lsw2_score_auto_keys();

/* Is the game's score screen (state 0x2C) up right now?
 *
 * WHY ANYONE ELSE NEEDS TO ASK. The session's "in a match" flag was driven by
 * `lsw2_cheats_in_arena_match()` alone, which is `level == 4 || level == 5` -
 * and `cheats.cpp` says so in its own words: *"This says the player is ON an
 * arena map, which is not the same as a round being in progress."*
 *
 * When a match ends the game unloads the level BEFORE entering the score
 * screen, so that predicate goes false one step early and the fork announced
 * "match ended, back to the lobby" and started the lobby countdown while the
 * score screen was still to come. MEASURED: the countdown line is printed above
 * the `[state] entered 44` line in the log.
 *
 * That inverts the required order - the score screen on its 15 s timer FIRST,
 * and only THEN everyone back in the lobby with its timer restarted - and
 * it lets a 30-second lobby timer expire while the
 * score screen is still on screen.
 *
 * So the score screen counts as part of the match, and this is how the session
 * asks. It reads through the same shared state chain as everything else, which
 * is the whole point of there being one. */
bool lsw2_score_screen_is_up();

/* Test seam: the pure half of the decision, so the timing and the one-shot rule
 * are provable without a guest.
 *
 * `now` and `entered` are the game's seconds counter. Returns true on the single
 * tick where the press should happen. */
bool lsw2_score_should_press(bool in_score_state, bool already_fired,
                             uint32_t now, uint32_t entered);

#endif /* LSW2_SCORESCREEN_H */
