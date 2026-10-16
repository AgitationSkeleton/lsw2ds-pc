/* The pure half of the host settings' Extras application - see extrasapply.h. */

#include "extrasapply.h"

bool lsw2_extras_should_write(uint32_t mask, uint32_t applied, bool in_fork) {
    /* THE BASELINE IS THE VANILLA GAME. The baseline build adds co-op over a
     * lobby list and nothing else; the host's Extras are a Bounty Hunter
     * match setting, and writing them into a plain co-op session would be the
     * fork leaking into the build that exists not to have it. */
    if (!in_fork) return false;

    /* ON ARRIVAL AS WELL AS ON CHANGE. `kLsw2ExtrasNever` is not a mask value -
     * eighteen flags cannot reach it - so it means "nothing written yet", and a
     * host whose Extras are all off still needs the array CLEARED once. Without
     * this the common case (mask 0, nothing applied) would compare equal and the
     * game would keep whatever Extras the save happened to carry. */
    if (applied == kLsw2ExtrasNever) return true;

    return mask != applied;
}
