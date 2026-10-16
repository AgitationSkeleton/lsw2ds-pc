/* The pure half of the arena's select skip - see selectskip.h. */

#include "selectskip.h"

bool lsw2_selectskip_should(bool in_fork, bool skips_select, int state_kind,
                            bool already) {
    /* THE BASELINE IS THE VANILLA GAME. The baseline build adds co-op
     * over a lobby list and nothing else; the arena's select belongs to the
     * fork's match rules. */
    if (!in_fork) return false;

    /* HUNTERS KEEPS ITS SELECT, with the usual 30s timer. This is the default
     * roster, so the ordinary case never reaches the writes below. */
    if (!skips_select) return false;

    if (state_kind != kLsw2StateSelect) return false;

    /* ONCE PER VISIT. The state completes on its next update, but "next" is
     * still a frame or two, and rewriting the same completion every one of them
     * would fight whatever the state does on its way out. */
    return !already;
}
