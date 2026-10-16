/* The slot-colour rule: the top-screen portrait ring is drawn in the player's slot colour. */

#include "slotcolour.h"

#include <cstdio>
#include <initializer_list>

static int g_checks = 0;
static int g_failed = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

int main() {
    /* The measured mapping: palettes 4,5,6,7 are slots 0..3. */
    check(lsw2_slot_palette(0) == 4, "slot 0 is palette 4 (blue)");
    check(lsw2_slot_palette(1) == 5, "slot 1 is palette 5 (green)");
    check(lsw2_slot_palette(2) == 6, "slot 2 is palette 6 (pink)");
    check(lsw2_slot_palette(3) == 7, "slot 3 is palette 7 (purple)");
    check(lsw2_slot_palette(4) == -1, "slot 4 does not exist");
    check(lsw2_slot_palette(-1) == -1, "a negative slot does not exist");

    /* THE REPORT'S OWN EXAMPLE: player two's POV wants green. */
    check(lsw2_slotcolour_rewrite(4, 1) == 5,
          "the blue ring becomes green for slot 1");

    /* A HOST CHANGES NOTHING - the game already draws blue. */
    for (int p = 4; p <= 7; ++p)
        check(lsw2_slotcolour_rewrite(p, 0) == -1, "slot 0 rewrites nothing");

    /* Already-correct sprites are left alone, so the log reports changes. */
    check(lsw2_slotcolour_rewrite(5, 1) == -1, "green stays green for slot 1");
    check(lsw2_slotcolour_rewrite(7, 3) == -1, "purple stays purple for slot 3");

    /* OUT OF RANGE PALETTES ARE NOT OURS. This is the bound that keeps the
     * hearts, the world and everything else untouched. */
    for (int p : {0, 1, 2, 3, 8, 9, 15})
        check(lsw2_slotcolour_rewrite(p, 2) == -1,
              "palettes outside 4..7 are left alone");

    check(lsw2_slotcolour_rewrite(4, -1) == -1, "no session, no rewrite");
    check(lsw2_slotcolour_rewrite(4, 9) == -1, "an impossible slot is refused");

    std::printf("%s: %d checks, %d failed\n",
                g_failed ? "FAIL" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
