/* The pure half of scorescreen.cpp, split for the same reason
 * customization_policy.cpp is: the timing rule and the one-shot rule are the
 * parts most worth pinning, and they must be testable without the ROM. The
 * other file reads guest memory and cannot link into a unit test.
 */

#include "scorescreen.h"

bool lsw2_score_should_press(bool in_score_state, bool already_fired,
                             uint32_t now, uint32_t entered) {
    if (!in_score_state || already_fired) return false;
    /* UNSIGNED SUBTRACTION, ON PURPOSE. The counter is a free-running word; if
     * it wraps between entering and expiring, `now - entered` is still the
     * elapsed count. Comparing `now >= entered + 15` would not be - it would
     * hang the screen forever on the one visit that straddles the wrap. */
    return (now - entered) >= uint32_t(kLsw2ScoreSeconds);
}

