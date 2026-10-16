/* The pure half of the Enhanced controls movement lever - see enhanced.h.
 *
 * Split from the guest half for the reason customization_policy.cpp and
 * lobbytimer.cpp are: these rules decide a control scheme and the precedence
 * between two players' settings, and both are worth testing without a ROM, a
 * frame or a running match. Nothing in this file includes the framework. */

#include "enhanced.h"

bool lsw2_enhanced_effective(bool host_allows, bool client_wants) {
    /* AND, and the requirement's asymmetry is about who may overrule whom
     * rather than about the operator. A client's Off overrides a host's
     * Enhanced: that is the false-wins half; a host who disallows it still
     * disallows it: that is the other. Both are the same conjunction read from
     * opposite ends. */
    return host_allows && client_wants;
}

int lsw2_enhanced_view_write(bool in_arena, bool enhanced, int current,
                             bool owned) {
    /* THE LOBBY KEEPS ITS OWN CONTROLS. Enhanced controls apply in arena
     * gameplay only; the lobby keeps the usual controls. The lever is
     * per-actor, so scoping it is just this test. */
    if (!in_arena) return -1;

    if (enhanced) {
        /* ONLY FROM THE SENTINEL, and this is the line that makes the whole
         * file correct without first measuring what view index the arena
         * assigns.
         *
         * If the arena assigns 4, the actor has no view of its own and the
         * offset is forced to zero - tank movement - and giving it the first
         * view is the feature. If the arena instead assigns a real 0-3 per
         * station, movement is ALREADY camera-relative and there is nothing to
         * change; overwriting it with 0 would point every player's movement at
         * the wrong camera in exactly the case where the game had it right.
         *
         * So: change the sentinel, never a real index. */
        if (current == kLsw2ViewSentinel) return kLsw2ViewLocal;
        return -1;
    }

    /* UNDO ONLY WHAT WE DID. A player turning Enhanced off mid-match should get
     * tank movement back, and the way to do that is to put the sentinel back -
     * but only on an actor whose index this port replaced. Restoring an index
     * the game chose would be inventing tank controls for a game that never
     * asked for them. */
    if (owned && current == kLsw2ViewLocal) return kLsw2ViewSentinel;
    return -1;
}
