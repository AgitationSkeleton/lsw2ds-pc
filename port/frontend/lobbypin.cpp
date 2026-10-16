/* See lobbypin.h. Pure - no guest, no framework. */

#include "lobbypin.h"

#include "whsession.h"   /* kLsw2MatchState, kLsw2PreGameState */

bool lsw2_lobbypin_should_warp_back(int level, int lobby_level, bool is_arena,
                                    bool cooling) {
    if (level == lobby_level) return false;   /* already home */

    /* AN ARENA IS WHERE THE MATCH HAPPENS. This is the clause the backstop was
     * missing, and its absence would have made every round unplayable. */
    if (is_arena) return false;

    /* THE COOLDOWN IS NOT POLITENESS: the request takes several frames to be
     * acted on, and during those frames the level still reads as the old one,
     * so an uncooled version posts a new request every frame into a state
     * machine already building one - the documented way to crash the guest. */
    if (cooling) return false;

    return true;
}

bool lsw2_lobbypin_is_quit(int level, bool in_session, bool match_running) {
    if (!in_session) return false;

    /* A LEVEL LOAD ALSO HAS NO LEVEL, and that is what this exists to survive.
     *
     * The rule below reads "off-level while connected means the player quit",
     * and it was true while the only way to be off-level was the front end.
     * Starting a match made it false: the transition out of the cantina into
     * the arena passes through `level -1` for about one and a half seconds -
     * measured at 1.43 s, 1.30 s and 1.06 s across three runs - and this check
     * runs every frame.
     *
     * When it sampled inside that window it shut the session down mid-match:
     *
     *     [fork] JOIN GAP (HOST) state 41 level -1
     *     [fork] left the lobby while connected - disconnecting
     *     [fork] session end: WH_End returned 65535
     *     [fork] lobby shut down          ... then 65 unmapped reads
     *
     * That is the fork ending its own match, and because it is a race against a
     * 1.4-second window it did so in roughly half of otherwise identical runs -
     * which is exactly why it was first, and wrongly, blamed on an unrelated
     * change.
     *
     * The match states are the discriminator. Quit Game goes to the front end
     * and STAYS there; a match transition is off-level only while the arena
     * loads, and the state machine is in the match the whole time. */
    if (match_running) return false;

    return level < 0;
}

int lsw2_lobbypin_settle_state(int state_now, int last_real) {
    /* See lobbypin.h. The first fix for the mid-match disconnect tested the
     * CURRENT state against the match states, and run 2 of its own A/B showed
     * why that is not enough:
     *
     *     1346  [state] entered -1   trail: ... 54 -> -1 -> 41 -> -1
     *     1348  [fork] left the lobby while connected - disconnecting
     *     1395  [state] entered 42
     *
     * The watchdog fires INSIDE the gap, where the state is -1 and so matches
     * neither 41 nor 42. Carrying the last real state across the gap is what
     * makes the question answerable at the moment it is actually asked. */
    return state_now >= 0 ? state_now : last_real;
}

bool lsw2_lobbypin_match_bound(int last_real, int pending) {
    /* See lobbypin.h. Either end of the transition names the match: the state
     * it is in, or the state it has been told to go to. */
    const auto is_match = [](int s) {
        return s == kLsw2MatchState || s == kLsw2PreGameState;
    };
    return is_match(last_real) || is_match(pending);
}
