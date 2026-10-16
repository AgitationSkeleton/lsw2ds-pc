/* The Enhanced controls' movement lever, tested without a ROM - see enhanced.h.
 *
 * The point of most of these is the pair of things the policy must get right
 * that no amount of playing would reveal quickly: the precedence between two
 * players' settings, and that the write is a no-op in the case that could not
 * be measured. A wrong precedence looks like "the setting sometimes works" and
 * a wrong no-op looks like "movement is broken for everyone but the host". */

#include "enhanced.h"

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        /* The runner counts lines that start with whitespace then "ok" to
         * report a check total, so a test that only speaks up on failure
         * reports "pass" with no number and looks emptier than it is. */
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

int main() {
    /* --- the precedence rule, all four combinations ---------------------- */
    check(lsw2_enhanced_effective(true, true),
          "host allows and client wants -> enhanced");
    check(!lsw2_enhanced_effective(true, false),
          "A CLIENT'S OFF BEATS A HOST'S ENHANCED - the specified case");
    check(!lsw2_enhanced_effective(false, true),
          "a client's On does NOT beat a host's Tank - the reverse is not true");
    check(!lsw2_enhanced_effective(false, false),
          "neither wants it -> tank");

    /* --- the lobby is untouched ------------------------------------------ */
    check(lsw2_enhanced_view_write(false, true, kLsw2ViewSentinel, false) < 0,
          "outside an arena the byte is left alone even with enhanced on");
    check(lsw2_enhanced_view_write(false, false, kLsw2ViewLocal, true) < 0,
          "...and even when we own it - the lobby keeps its usual controls");

    /* --- the feature ----------------------------------------------------- */
    check(lsw2_enhanced_view_write(true, true, kLsw2ViewSentinel, false)
              == kLsw2ViewLocal,
          "an arena actor with the sentinel gets the first view");

    /* --- THE CASE THAT COULD NOT BE MEASURED -----------------------------
     * If the arena already assigns a real per-station index, movement is
     * already camera-relative and the port must not touch it. This is what
     * makes the whole feature shippable before the measurement rather than
     * after it, so it is worth more than one check. */
    for (int real = 0; real < 4; ++real) {
        check(lsw2_enhanced_view_write(true, true, real, false) < 0,
              "a REAL view index is never overwritten - nothing to change");
    }

    /* --- the undo, and its limit ----------------------------------------- */
    check(lsw2_enhanced_view_write(true, false, kLsw2ViewLocal, true)
              == kLsw2ViewSentinel,
          "turning enhanced off restores tank movement on an actor we changed");
    check(lsw2_enhanced_view_write(true, false, kLsw2ViewLocal, false) < 0,
          "...but NOT on an index the game chose - that would invent tank "
          "controls for a game that never asked for them");
    check(lsw2_enhanced_view_write(true, false, kLsw2ViewSentinel, true) < 0,
          "an actor already on the sentinel is not written again");

    /* --- idempotence, because this runs sixty times a second -------------- */
    check(lsw2_enhanced_view_write(true, true, kLsw2ViewLocal, true) < 0,
          "once the write has landed the policy stops asking for it");

    /* --- the two constants are actually different ------------------------
     * Trivial, and it is here because the entire feature is the difference
     * between them; a merge that made them equal would silently turn every
     * check above into a tautology. */
    check(kLsw2ViewSentinel != kLsw2ViewLocal,
          "the sentinel and the local view are distinct values");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
