/* THE FREE PLAY GRID AS A PICKER, in two places:
 *
 *  1. THE HOST'S ONE CHARACTER. The one-character picker on the host's options
 *     menu uses the Free Play grid instead of a long text list, and the
 *     character picked there is the one the match uses.
 *
 *  2. THE PRE-GAME PICK UNDER ALL. When any character is allowed (not just
 *     hunters), the pre-game pick shows the game's own touch-screen Free Play
 *     menu in place of the usual statue select - except that B cannot exit
 *     the menu. The select's usual timer applies, and the character is
 *     confirmed and locked the same way as the Free Play pre-level pick, with
 *     A or a double tap.
 *
 * BOTH ARE THE CARTRIDGE'S OWN WIDGET (0x020682C4), and it lives in exactly one
 * place: state 48's touch-screen tab UI. The factory's kind 48 builds the tab
 * state (0x0205F85C, vtable 0x020FF414), and that state's enter, 0x02087390,
 * builds the widget into its +0xC4. The statue select (41) replaces 48 in the
 * same screen slot, so the grid cannot run beside it. Both pickers therefore
 * run where the grid already is - in the lobby - and both widen it to every
 * page but the Character Creator's (selectwidget.h's kLsw2SelectAllPageHigh,
 * the one place that limit is decided and can be lifted).
 *
 * THE ONE POINT A CONFIRM CAN BE SEEN. The widget's tap handler asks whether
 * the tapped character is unlocked - `0206887C bl 0x02086534`, returning to
 * 0x02068880 - before it compares the tap with its selection (0x02068908) and
 * confirms on a match. That call crosses banks, so an override on
 * 0x02086534 fires for it, with the widget in r6 and the tapped entry in r5.
 * The host's picker records the pick there and turns the confirm into a plain
 * highlight, so the host's own character never changes. The pre-game pick
 * lets the confirm run (it is the player's own switch, message 7 in a session)
 * and locks every later tap.
 */

#ifndef LSW2_FREEPLAYPICK_H
#define LSW2_FREEPLAYPICK_H

#include <cstdint>

/* --- 1. The host's One character ------------------------------------------ */

/* Arm and disarm the host's picker. fork.cpp does both, following the fork
 * session's Character screen, which no longer opens a menu of its own. */
void lsw2_freeplaypick_host_begin();
void lsw2_freeplaypick_host_end();
bool lsw2_freeplaypick_host_active();

/* The character the host confirmed with a double tap, or -1. Clears it. */
int lsw2_freeplaypick_host_take();

/* B while picking: true once, then cleared. */
bool lsw2_freeplaypick_host_cancelled();

/* --- 2. The pre-game pick under ALL, in a shared session ------------------- */

/* From the state-request override (statereq.cpp), with the request's
 * registers live. The round start asks for the Walkaround world (from
 * 0x02072DC4) and then for state 41 (from 0x02072DE0) on every console at one
 * step. Under ALL this starts the pick instead, by rewriting the pair into the
 * lobby return's own - the world in CoopEnter, the screen 48 - so every
 * console reloads the lobby, grid and all, at the same step everywhere. */
void lsw2_freeplaypick_round_start_request();

/* The select's own 30 seconds, counted down on the game's clock; -1 when no
 * pick is running. For the HUD. */
int lsw2_freeplaypick_pregame_seconds_left();

/* Whether the pre-game pick is running on this console - from the round
 * start's request to the end effect's apply. The pick is the round start, not
 * the lobby, although it runs in the lobby's screen state: vwh.cpp seats no
 * joiner during it, as it seats none during a match. */
bool lsw2_freeplaypick_pregame_active();

/* THE EFFECT'S APPLY (vwh.h kLsw2EffectPickEnd), at one dataset on every
 * console: the pick is over, and the round start the pick was standing in
 * for is asked for now - the same Walkaround world and state 41 - so the
 * cartridge's select runs (ALL completes it at once, sessionroster.cpp) and
 * builds the arena. */
void lsw2_freeplaypick_pregame_end();

/* THE HOST'S RECORD of what each station picked, taken in the lobby just
 * before the round start is asked for - after it, the Walkaround reload
 * rebuilds every party actor as character 0, and a read then gives 0 for
 * everybody (measured). -1 when there is no pick for the station. */
int lsw2_freeplaypick_round_pick(int station);

/* netmsglog.cpp names each message once as it arrives. A Free Play switch is
 * type 7; during the pick the host counts the stations that sent one. */
void lsw2_freeplaypick_note_message(int station, int type);

/* --- Both ------------------------------------------------------------------ */

/* Once a frame from the fork tick: the host's end of the pick. */
void lsw2_freeplaypick_tick();

/* The frame's keys (active HIGH) as the guest should see them while a picker
 * is up. The host's picker is modal like the menu it replaces: L and R page,
 * B backs out, and nothing else reaches the lobby - its confirm is the double
 * tap alone (see characterpick.h). The pre-game pick keeps B from the guest
 * (no exit) and turns A into the confirm (A or a double tap confirms). */
uint16_t lsw2_freeplaypick_filter_keys(uint16_t keys);

/* A tap the picker is making for the player: the pre-game's A confirms the
 * highlighted portrait by tapping it a second time - the widget's own
 * confirm, so the switch is the game's. 0 = nothing this frame, 1 = press at
 * (x, y), 2 = release. */
int lsw2_freeplaypick_touch(uint16_t* x, uint16_t* y);

/* The override on 0x02086534 (kOverridesCore). */
void lsw2_freeplaypick_unlock_query();

#endif /* LSW2_FREEPLAYPICK_H */
