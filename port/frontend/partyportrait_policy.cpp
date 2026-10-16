/* The pure half of the top-screen portrait refresh - see partyportrait.h.
 *
 * Nothing here includes the framework, so `test_partyportrait.cpp` can assert
 * the rule without a ROM, a frame or a guest. The rule is small and the reason
 * to test it is that its FIRST call is the one that matters: a refresh fired on
 * the first observation would run every time the widget appears, whether or not
 * anything changed. */

#include "partyportrait.h"

bool lsw2_partyportrait_changed(const uint8_t* now, const uint8_t* before,
                                int count, bool have_before) {
    if (!now || !before || count <= 0) return false;

    /* THE FIRST OBSERVATION ESTABLISHES THE BASELINE AND FIRES NOTHING.
     * Before it there is no "was", so every slot would read as changed and the
     * refresh would run on a party nobody had touched - a fix that always
     * appears to work, including when it is doing nothing. */
    if (!have_before) return false;

    /* A SLOT GOING EMPTY IS THE SWITCH TEARING DOWN, NOT A NEW PARTY.
     *
     * MEASURED by driving a Free Play switch with a double
     * tap and watching the party either side of it:
     *
     *     [fork] portrait: the party changed and the refresh ran
     *     [fork] party portrait refreshed: slots 100 100 100 100
     *     [fork] party slots now 7 100 100 100 (no refresh; ...)
     *
     * The party passes through an ALL-EMPTY frame between the old actor being
     * torn down and the new one existing. This rule fired on that frame, the
     * refresh ran against an empty party, and by the time the real answer -
     * character 7 - arrived, the change had been spent. So the one refresh the
     * switch gets was aimed at nothing, and the settled character never got
     * one.
     *
     * So a slot that has gone from a character to EMPTY carries no news. A slot
     * that has GAINED a character, or swapped one for another, is the event
     * this exists to notice. Same distinction customization.cpp had to make for
     * the customize block, and for the same reason: the game clears its own
     * state mid-switch, and reading that clear as a choice is what breaks the
     * feature.
     *
     * A PARTY THAT LEGITIMATELY EMPTIES - leaving the level - therefore fires
     * nothing, which is right: there is no portrait to refresh on the way out,
     * and the next arrival establishes its own baseline. */
    for (int i = 0; i < count; ++i) {
        if (now[i] == before[i]) continue;
        if (now[i] == kLsw2PartyEmpty) continue;   /* went empty: the teardown */
        return true;
    }
    return false;
}
