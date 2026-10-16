/* The Display Swap mapping, pinned.
 *
 * This is four cases and it would be tempting to skip. It is tested because the
 * bug it replaces was invisible: two files read a hardcoded engine index and
 * got back a real OAM image belonging to the other screen, so every sprite in
 * it decoded correctly and described the wrong picture. Getting the polarity
 * backwards here would reproduce that exactly, with a function in the way
 * making it look considered.
 */

#include "screenengine.h"

#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    /* The hardware's own wording: POWCNT1 bit 15, "Display Swap (0 = Engine A
     * to Lower Screen, 1 = Engine A to Upper Screen)". */
    const unsigned swapped = kLsw2PowcntDisplaySwap;   /* A on top    */
    const unsigned normal = 0u;                        /* A on bottom */

    std::printf("bit 15 set - engine A drives the upper screen\n");
    check("the top screen is the main engine",
          lsw2_screen_engine_for(swapped, kLsw2ScreenTop) == kLsw2EngineMain);
    check("and the touch screen is the sub engine",
          lsw2_screen_engine_for(swapped, kLsw2ScreenTouch) == kLsw2EngineSub);

    std::printf("bit 15 clear - engine A drives the lower screen\n");
    check("the top screen is the SUB engine",
          lsw2_screen_engine_for(normal, kLsw2ScreenTop) == kLsw2EngineSub);
    check("and the touch screen is the MAIN engine",
          lsw2_screen_engine_for(normal, kLsw2ScreenTouch) == kLsw2EngineMain);

    std::printf("the two screens never share an engine\n");
    check("with the bit set",
          lsw2_screen_engine_for(swapped, kLsw2ScreenTop) !=
              lsw2_screen_engine_for(swapped, kLsw2ScreenTouch));
    check("and with it clear",
          lsw2_screen_engine_for(normal, kLsw2ScreenTop) !=
              lsw2_screen_engine_for(normal, kLsw2ScreenTouch));

    std::printf("the rest of POWCNT1 is ignored\n");
    /* 0x820F is what the runner latches at boot - LCD, both 2D engines, the
     * geometry and rendering engines - and only bit 15 of it may matter here. */
    check("0x820F puts engine A on top, like bare 0x8000",
          lsw2_screen_engine_for(0x820Fu, kLsw2ScreenTop) ==
              lsw2_screen_engine_for(swapped, kLsw2ScreenTop));
    check("0x020F puts it on the bottom, like bare 0",
          lsw2_screen_engine_for(0x020Fu, kLsw2ScreenTop) ==
              lsw2_screen_engine_for(normal, kLsw2ScreenTop));

    std::printf("a screen index that is not a screen\n");
    check("falls back to the main engine rather than an engine that is not there",
          lsw2_screen_engine_for(normal, 7) == kLsw2EngineMain);
    check("and does so whichever way the bit is set",
          lsw2_screen_engine_for(swapped, -1) == kLsw2EngineMain);

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all checks passed");
    return g_failures ? 1 : 0;
}
