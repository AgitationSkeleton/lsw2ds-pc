/* The cantina lobby's exit door is disabled, so the player cannot leave the
 * room - and this is the one rule that decides when the fork drags the player
 * back.
 *
 * WHY THIS IS ITS OWN FILE. The rule lives in two places that have to agree:
 *
 *   the PIN      rewrites a pending level request back to the lobby
 *   the BACKSTOP warps back when the pin never saw the transition at all
 *
 * Once they did NOT agree. The pin had been given an arena exemption
 * when the lobby console's round start was built - a match is a level change
 * the fork wants - and the backstop had not. So the first round ever to start
 * would have warped the player straight back out of Jabba's Tower, once per
 * cooldown, for the whole match. The entire match flow, broken by a missing
 * else-if.
 *
 * Nothing caught it because it had never been reachable: the arena lobby's
 * console entity refused to open until the flag-11 fix, so no match could
 * begin. It is the same shape as the pause fixups that could not fire -
 * correct only because the situation that breaks it does not exist yet.
 *
 * So the rule is stated once, here, and tested. Two copies of a "should we drag
 * the player back" decision is exactly how one of them stays right while the
 * other is the one that runs. */

#ifndef LSW2_LOBBYPIN_H
#define LSW2_LOBBYPIN_H

/* Should the fork warp the player back to the arena lobby?
 *
 * `level` is what lsw2_cheats_current_level() answers - NEGATIVE when the
 * context is not on a level at all, which is where Quit Game lands.
 *
 * Returns false for the lobby (already home), false for an arena (the match),
 * and false while the cooldown is unexpired. */
bool lsw2_lobbypin_should_warp_back(int level, int lobby_level, bool is_arena,
                                    bool cooling);

/* The door leads to another LEVEL; Quit Game goes to the front end, where the
 * level index is not a level index. Disconnecting a host for walking through a
 * door would be a worse bug than the one the disconnect fixes. */
/* Is this departure the pause menu's Quit Game rather than the door - or a
 * level LOAD, which also has no level?
 *
 * `match_running` is true while the cartridge's match states are active. A
 * match transition passes through "no level" for about 1.4 seconds while the
 * arena loads, and this predicate runs every frame, so without that input it
 * shut the session down mid-match in roughly half of all runs. See lobbypin.cpp
 * for the measured window and the log it produced. */
bool lsw2_lobbypin_is_quit(int level, bool in_session, bool match_running);

/* Carry the last REAL state across the gap a level load opens.
 *
 * The state machine reports -1 while a level loads - "no current state object" -
 * at the same time as the level index reads -1, so a caller asking "is the match
 * running?" during the arena transition sees neither the match state nor a
 * level. The host's trail is literally `41 -> -1 -> 42`, and the watchdog that
 * ended the session fired in that gap.
 *
 * Returns `state_now` when it is a real state, and `last_real` while it is
 * negative, so a transient gap continues whatever was running before it. Pure,
 * so the caller owns the storage and this stays testable. */
int lsw2_lobbypin_settle_state(int state_now, int last_real);

/* Is the match running, OR HAS THE MACHINE ALREADY BEEN ASKED FOR IT?
 *
 * `last_real` is lsw2_lobbypin_settle_state's answer and `pending` is
 * lsw2_state_pending()'s - the screen slot's accepted-but-unbuilt request.
 *
 * The carried state alone covers the arena load (41 -> -1 -> 42) and NOT the
 * round start before it. There the cartridge asks for the lobby level in the
 * world slot and for 41 in the screen slot in one go (FUN_02072CF4 mode 3), and
 * the world unloads while the screen is still the lobby (48) or Game Settings
 * (54) - so for that window "no level" and "not in a match state" are both true
 * and the quit rule fired on the host and on a joiner in the same four-console
 * run. The pending 41 is the round start's own decision, readable
 * the whole time it is in flight. */
bool lsw2_lobbypin_match_bound(int last_real, int pending);

#endif /* LSW2_LOBBYPIN_H */
