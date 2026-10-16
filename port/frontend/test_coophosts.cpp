/* The host-table rules, asserted without a ROM.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_coophosts.cpp \
 *       port/frontend/coophosts.cpp -o test_coophosts && ./test_coophosts
 */

#include "coophosts.h"

#include <cstdio>

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
}  // namespace

int main() {
    std::printf("coop host table\n");

    check(kLsw2HostSlots == 16, "sixteen slots, as the count loop bounds it");
    check(kLsw2HostStride == 0xC4u, "0xC4 apart");

    check(lsw2_coophosts_rows(0) == 0, "no lobbies, no rows");
    check(lsw2_coophosts_rows(-1) == 0, "and a negative count is not a row");
    check(lsw2_coophosts_rows(3) == 3, "three lobbies, three rows");
    check(lsw2_coophosts_rows(16) == 16, "a full table");
    check(lsw2_coophosts_rows(40) == 16,
          "and MORE than the table is clamped, not wrapped");

    /* The status must not be the value the list skips. */
    check((lsw2_coophosts_status() & 3u) != 2u,
          "the published status is not the one the list skips");
    check(lsw2_coophosts_status() != 0u,
          "and not zero, which is what an untouched record holds");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
