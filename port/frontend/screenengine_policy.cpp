/* See screenengine.h - the half that is arithmetic on POWCNT1 and nothing else.
 *
 * SPLIT FOR THE SAME REASON customization_policy.cpp and scorescreen_policy.cpp
 * are: `test_screenengine` links this file alone. The wrapper that actually
 * asks the runner for the register lives in screenengine.cpp, so a test of the
 * mapping does not pull io.h and the whole framework onto its link line.
 */

#include "screenengine.h"

int lsw2_screen_engine_for(unsigned powcnt1, int screen) {
    /* BIT 15 SET MEANS ENGINE A IS ON THE UPPER SCREEN. Written out as the two
     * cases rather than as arithmetic on the bit, because the polarity is the
     * whole content of this function and a `^` would hide it.
     *
     * The framework's own presenter computes the same thing as
     * `engine = a_on_top ? screen : (screen ^ 1)` (gpu2d.cpp), which is where
     * the polarity was checked rather than reasoned about. */
    const bool a_on_top = (powcnt1 & kLsw2PowcntDisplaySwap) != 0u;

    if (screen == kLsw2ScreenTop)
        return a_on_top ? kLsw2EngineMain : kLsw2EngineSub;
    if (screen == kLsw2ScreenTouch)
        return a_on_top ? kLsw2EngineSub : kLsw2EngineMain;

    /* Not a screen. Hand back what the two callers assumed before this existed,
     * so a bad index degrades to the old behaviour instead of indexing an
     * engine that is not there. */
    return kLsw2EngineMain;
}

uint32_t lsw2_screen_dispcnt_for(unsigned powcnt1, int screen) {
    return lsw2_screen_engine_for(powcnt1, screen) == kLsw2EngineMain
               ? kLsw2DispcntEngineA
               : kLsw2DispcntEngineB;
}
