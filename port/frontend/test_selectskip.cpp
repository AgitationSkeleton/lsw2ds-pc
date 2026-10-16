/* The host settings' select skip, tested without a guest - see selectskip.h. */

#include "selectskip.h"

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

int main() {
    const int sel = kLsw2StateSelect;

    /* --- the feature ------------------------------------------------------ */
    check(lsw2_selectskip_should(true, true, sel, false),
          "a non-Hunters roster in the select completes it");

    /* --- Hunters keeps its select, and it is the DEFAULT ------------------- */
    check(!lsw2_selectskip_should(true, false, sel, false),
          "HUNTERS KEEPS THE SELECT - its usual 30s timer, as specified");

    /* --- the baseline is untouched ---------------------------------------- */
    check(!lsw2_selectskip_should(false, true, sel, false),
          "the baseline never skips - the arena's rules are the fork's");

    /* --- only in that state ----------------------------------------------- */
    check(!lsw2_selectskip_should(true, true, 0x36, false),
          "the map/gamemode/time chooser is left alone - it is specified "
          "unconditionally");
    check(!lsw2_selectskip_should(true, true, 0x2C, false),
          "the score screen is left alone");
    check(!lsw2_selectskip_should(true, true, -1, false),
          "no select state means nothing to do");

    /* --- once per visit ---------------------------------------------------
     * The state completes on its NEXT update, so the fork sees itself in the
     * select for a frame or two after writing. Rewriting the completion every
     * one of those frames would fight the state on its way out. */
    check(!lsw2_selectskip_should(true, true, sel, true),
          "having already written it, the fork does not write it again");

    /* --- the two constants are distinct ----------------------------------- */
    check(kLsw2SelPicked != kLsw2SelChosen,
          "the count and the per-slot flags are different fields");
    check(kLsw2SelSlots == 4, "four slots, matching the arena's stations");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
