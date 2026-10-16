/* The party-portrait rule: when should the refresh run.
 *
 * Small, and worth pinning because its FIRST call is the one that matters. A
 * refresh fired on the first observation would run every time the Free Play
 * screen opens, on a party nobody had touched - which is a fix that always
 * appears to work, including when it is doing nothing. */

#include "partyportrait.h"

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

int main() {
    const uint8_t a[4] = {7, 100, 100, 100};
    const uint8_t b[4] = {29, 100, 100, 100};

    check(!lsw2_partyportrait_changed(a, b, 4, false),
          "the first observation fires nothing, even when the arrays differ");
    check(lsw2_partyportrait_changed(a, b, 4, true),
          "Greedo to Jango in slot 0 is a change");
    check(!lsw2_partyportrait_changed(a, a, 4, true),
          "an unchanged party is not a change");

    /* EVERY SLOT COUNTS, not just the local player's - the motivating
     * example is switching your own character, but a partner swapping is the
     * same stale portrait one row down. */
    for (int i = 0; i < 4; ++i) {
        uint8_t now[4] = {1, 2, 3, 4};
        uint8_t before[4] = {1, 2, 3, 4};
        now[i] = 99;
        check(lsw2_partyportrait_changed(now, before, 4, true),
              "a change in any slot counts");
    }

    /* THE TRANSIENT EMPTY FRAME, measured. A Free Play switch tears the old
     * actor down before the new one exists, so the party passes through
     * all-empty. Firing there spends the switch's one refresh on nothing and
     * the settled character never gets one. */
    {
        const uint8_t settled[4] = {20, 100, 100, 100};
        const uint8_t teardown[4] = {100, 100, 100, 100};
        const uint8_t arrived[4] = {7, 100, 100, 100};
        check(!lsw2_partyportrait_changed(teardown, settled, 4, true),
              "a slot going empty mid-switch is the teardown, not a change");
        check(lsw2_partyportrait_changed(arrived, teardown, 4, true),
              "the new character arriving IS the change");
        check(lsw2_partyportrait_changed(arrived, settled, 4, true),
              "and a straight character-to-character swap still counts");
    }

    check(!lsw2_partyportrait_changed(nullptr, b, 4, true), "null now is safe");
    check(!lsw2_partyportrait_changed(a, nullptr, 4, true), "null before is safe");
    check(!lsw2_partyportrait_changed(a, b, 0, true), "zero count is safe");
    check(!lsw2_partyportrait_changed(a, b, -1, true), "negative count is safe");

    std::printf("%s: %d checks, %d failed\n",
                g_failed ? "FAIL" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
