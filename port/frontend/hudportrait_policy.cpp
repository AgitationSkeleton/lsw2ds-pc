/* The top-screen HUD portrait's rebuild rule, split out from hudportrait.cpp so it can be tested without a
 * ROM or a guest - the same split customization_policy.cpp and
 * partyportrait_policy.cpp already make.
 *
 * The rest of hudportrait.cpp reads guest memory and queues guest calls, none
 * of which a unit test can have. This decision does not, and it is the one that
 * needs pinning: it guards a teardown-and-rebuild, so getting it wrong is heap
 * churn every frame rather than a wasted redraw. */

#include "hudportrait.h"
#include "partyportrait.h"   /* kLsw2PartyEmpty */

bool lsw2_hudportrait_should_rebuild(uint8_t now, uint8_t before,
                                     bool have_before) {
    /* THE FIRST OBSERVATION ESTABLISHES A BASELINE. Firing here would rebuild
     * the portrait the first time the tick ever runs, for a character nobody
     * changed - a fix that always appears to work, including when it is doing
     * nothing. */
    if (!have_before) return false;
    if (now == before) return false;

    /* THE TEARDOWN FRAME IS NOT A CHANGE. A Free Play switch was measured
     * removing the old actor before the new one exists, so the character passes
     * through the empty value on the way. Rebuilding there would spend the
     * switch on nothing and the settled character would never get one.
     *
     * This is the game's own guard, not an invention: FUN_0208c024 returns
     * early when the character it is handed is 100. */
    if (now == kLsw2PartyEmpty) return false;
    return true;
}
