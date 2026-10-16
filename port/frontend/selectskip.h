/* Any Roster setting besides Hunters skips the character select phase of the
 * arena flow.
 *
 * WHY THIS IS NOT A SELECTOR CHOICE, which is where two earlier passes looked.
 * Reading the round-start chain found the two selector values are a
 * chain rather than a choice: 1 opens the map/gamemode/time chooser, and the
 * chooser BROADCASTS ITS OWN 3 at `0x0209E6D8` once the host confirms. Selector
 * 3 is what reaches the statue select. So by the time the select is about to
 * appear, the fork's selector is long spent and the game is asking for the
 * select on its own behalf.
 *
 * The flow wants the chooser unconditionally and the select only for
 * Hunters, so the skip has to act on the SELECT rather than on the request that
 * summoned it.
 *
 * HOW THE SELECT ENDS, from reading the state's update through:
 *
 *     +0x2B10[slot]  the spinner index this player is on
 *     +0x2B14[slot]  1 once that player has chosen
 *     +0x2B06        how many have chosen, compared against the player count
 *
 * A player confirming does `+0x2B10[slot] = index`, `+0x2B14[slot] = 1`, a
 * redraw, then `+0x2B06 += 1`. When the count reaches the player count the
 * state moves on. So the fork does not need to suppress the screen or fake a
 * button press - it fills in the game's own bookkeeping and the state completes
 * itself on its next update. Same shape as the level warp and the page bounds:
 * fill in the game's data and let the game's code act on it.
 *
 * WHAT IS DELIBERATELY NOT WRITTEN. `+0x2B10`, the spinner index. The game
 * initialises it, and on completion it writes each player's chosen statue into
 * the save as their character - so putting a value there would be the fork
 * choosing characters behind the roster rules that already own that decision
 * (rosterenforce.cpp). Marking players as done leaves the game's own default
 * standing, and for every roster that skips the select, the character is
 * assigned afterwards anyway.
 *
 * ONE FRAME, NOT ZERO. The select state is entered and then completes on its
 * next update, so it may be visible for a frame. Suppressing its construction
 * would need the request intercepted before the game made it, which is the
 * thing the round-start reading established cannot be done from the fork's
 * side of the chain.
 * A frame is the honest cost and is recorded here rather than glossed.
 */

#ifndef LSW2_SELECTSKIP_H
#define LSW2_SELECTSKIP_H

#include "statelog.h"

#include <cstdint>

/* The statue select's state kind is statelog.h's kLsw2StateSelect - this
 * header used to declare 0x29 itself, and diagnostics.cpp and
 * lasthunter_observe.cpp each declared it a third and fourth time as
 * kStateHunterSelect. The statue select and the hunter select are the same
 * screen; the two names are why it did not read as one fact. */
constexpr uint32_t kLsw2SelPicked = 0x2B06u;
constexpr uint32_t kLsw2SelChosen = 0x2B14u;   /* [4], u8 */
constexpr int kLsw2SelSlots = 4;

/* THE PURE RULE. `skips_select` is lsw2_roster_skips_select() on the host's
 * roster - the same predicate the round start uses, so the two cannot disagree
 * about what "besides Hunters" means.
 *
 * `already` stops the fork writing the same completion every frame of a state
 * that is on its way out anyway. */
bool lsw2_selectskip_should(bool in_fork, bool skips_select, int state_kind,
                            bool already);

/* Call once a frame. Silent in the baseline, outside the select, and when the
 * roster is Hunters - which is the default, so this is silent in the ordinary
 * case. */
void lsw2_selectskip_tick();

#endif /* LSW2_SELECTSKIP_H */
