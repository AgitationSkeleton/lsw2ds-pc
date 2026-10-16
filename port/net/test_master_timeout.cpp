/* Does an unreachable directory fail fast, or hang the frame it was called on?
 *
 *   g++ -std=c++20 port/net/test_master_timeout.cpp port/net/lsw2_master.cpp \
 *       -lws2_32 -o test_master_timeout
 *
 * WHY THIS IS A TEST AND NOT A COMMENT. Every call into the directory runs on
 * the frame thread. Before this, none of them had a timeout, so an address that
 * black-holes packets cost the OS default - about twenty seconds on Windows.
 *
 * The heartbeat is what turns that from annoying into unplayable: it fires
 * every sixty frames while hosting, so a directory going down mid-match would
 * freeze the game for twenty seconds, run for one, and freeze again for as long
 * as the lobby stayed up.
 *
 * A duration assertion is unusual and is justified here: the property being
 * protected IS a duration, and nothing else in the code expresses it. It is
 * generous - six seconds against a two-second budget - so it fails on the
 * regression it exists to catch and not on a slow machine.
 */

#include "lsw2_master.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#endif

#include <chrono>
#include <cstdio>

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::printf("master client timeouts\n");

    /* 10.255.255.1 is a private address that routes nowhere and, crucially,
     * BLACK-HOLES rather than refusing. A refused connection returns at once
     * and would prove nothing; this is the case that used to take the full OS
     * timeout. */
    Lsw2MasterClient c;
    c.configure("10.255.255.1", 28553);

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = c.refresh();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::printf("  refresh against a black hole -> %s in %lld ms\n",
                ok ? "ok" : "failed", ms);

    int failures = 0;
    if (ok) {
        std::printf("  FAIL it claimed to reach a black hole\n");
        ++failures;
    }
    if (ms >= 6000) {
        std::printf("  FAIL took %lld ms - the connect is unbounded again\n", ms);
        ++failures;
    } else {
        std::printf("  ok   returned promptly rather than hanging the frame\n");
    }

#ifdef _WIN32
    WSACleanup();
#endif
    if (failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
