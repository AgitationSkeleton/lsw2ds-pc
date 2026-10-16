/* The host settings' Extras application, tested without a guest - see extrasapply.h.
 *
 * Small, and the two cases that matter are the ones a running game would show
 * only by omission: the all-off host whose array never gets cleared, and the
 * baseline quietly inheriting a fork setting. */

#include "extrasapply.h"

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
    /* --- the baseline never writes ---------------------------------------- */
    check(!lsw2_extras_should_write(0x1234u, kLsw2ExtrasNever, false),
          "the baseline does not write even on first arrival");
    check(!lsw2_extras_should_write(0x1234u, 0u, false),
          "...nor when the mask differs - it is a fork setting");

    /* --- the first write, including the all-off host ---------------------
     * THE CASE THAT WOULD HAVE BEEN MISSED. A host with no Extras selected has
     * mask 0, and "nothing applied yet" would compare equal to it under a plain
     * `mask != applied`. The array would then keep whatever the save carried,
     * which is precisely the Extras the everything-unlocked start made
     * purchasable. */
    check(lsw2_extras_should_write(0u, kLsw2ExtrasNever, true),
          "an all-off host still CLEARS the array once");
    check(lsw2_extras_should_write(0x20001u, kLsw2ExtrasNever, true),
          "and a host with Extras writes on arrival");

    /* --- steady state ----------------------------------------------------- */
    check(!lsw2_extras_should_write(0x20001u, 0x20001u, true),
          "no write while the host's set is unchanged");
    check(lsw2_extras_should_write(0x20003u, 0x20001u, true),
          "a change writes");
    check(lsw2_extras_should_write(0u, 0x20001u, true),
          "turning everything off writes - Extras must not accumulate");

    /* --- the sentinel is not a reachable mask -----------------------------
     * Eighteen flags cannot produce kLsw2ExtrasNever, which is what lets it
     * double as "never written". If the count ever grew past 32 this would stop
     * being true, so it is pinned here rather than assumed. */
    check(kLsw2ExtrasNever > 0x3FFFFu,
          "the sentinel is outside the range eighteen flags can reach");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
