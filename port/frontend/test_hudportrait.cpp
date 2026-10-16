/* The HUD portrait rule: when should the TOP-screen portrait be rebuilt.
 *
 * Worth pinning separately from test_partyportrait.cpp, because this rule
 * guards something much more expensive than a redraw. A rebuild releases two
 * sprites through their own virtual destructors and allocates two more, so
 * firing it when nothing changed is not a wasted redraw - it is heap churn on
 * every frame, and a fix that does that would look like it works right up until
 * it does not.
 *
 * The transient-empty case is the one that actually bit this project. A Free
 * Play switch was measured tearing the old actor down before the new one
 * exists, so the character passes through the empty value on the way. Rebuilding
 * there would spend the switch on nothing, and the settled character would never
 * get its own rebuild - the exact shape of a fix that appears to run and changes
 * nothing on screen.
 */

#include "hudportrait.h"
#include "partyportrait.h"   /* kLsw2PartyEmpty */

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

int main() {
    /* THE FIRST OBSERVATION ESTABLISHES A BASELINE AND FIRES NOTHING. Without
     * this the portrait would be torn down and rebuilt the first time the tick
     * ever runs, on a character nobody had changed. */
    check(!lsw2_hudportrait_should_rebuild(7, 20, false),
          "the first observation fires nothing, even when the values differ");

    check(lsw2_hudportrait_should_rebuild(7, 20, true),
          "Greedo to Boba is a rebuild");
    check(!lsw2_hudportrait_should_rebuild(20, 20, true),
          "an unchanged character is not a rebuild");

    /* THE TEARDOWN FRAME. The game's own setter refuses this value too -
     * FUN_0208c024 returns early when the character is 100 - so this is not an
     * invention, it is the same guard. */
    check(!lsw2_hudportrait_should_rebuild(kLsw2PartyEmpty, 20, true),
          "the character going empty mid-switch is the teardown, not a change");
    check(lsw2_hudportrait_should_rebuild(7, kLsw2PartyEmpty, true),
          "the new character arriving after the empty frame IS the change");

    /* AND IT MUST STILL FIRE ON A DIRECT SWAP, because the empty frame is not
     * guaranteed - a switch that happens inside one tick goes straight from one
     * character to the next. */
    check(lsw2_hudportrait_should_rebuild(29, 7, true),
          "a straight character-to-character swap still counts");

    /* Every character in the nine-character switch set is a legitimate target; none of them may
     * be mistaken for the empty marker. */
    const uint8_t nine[9] = {7, 11, 15, 29, 60, 61, 63, 64, 65};
    for (int i = 0; i < 9; ++i) {
        check(lsw2_hudportrait_should_rebuild(nine[i], 20, true),
              "each of the nine is a real character to rebuild for");
        check(nine[i] != kLsw2PartyEmpty,
              "and none of them collides with the empty marker");
    }

    std::printf("%s: %d checks, %d failed\n",
                g_failed ? "FAIL" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
