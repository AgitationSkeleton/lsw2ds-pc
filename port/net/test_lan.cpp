/* LAN discovery, over real sockets on loopback.
 *
 *   g++ -std=c++20 port/net/test_lan.cpp port/net/lsw2_lan.cpp \
 *       port/net/lsw2_name.cpp port/net/lsw2_master.cpp -lws2_32 -o test_lan
 *
 * A LOOPBACK TEST IS A REAL TEST HERE. The thing that can go wrong with
 * discovery is not the record layout - it is the socket options - and those
 * only exist at runtime. SO_REUSEADDR is exactly what lets a host and a browser
 * share the port on one machine, which is both how this runs and the first
 * thing a person will try.
 */

#include "lsw2_lan.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#endif

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/* Announce, then drain. UDP on loopback is effectively immediate, but a few
 * attempts cost nothing and remove a source of flake that would be blamed on
 * the code rather than on the scheduler. */
void pump(Lsw2LanBeacon& beacon, Lsw2LanBrowser& browser, int players,
          int max_players = 4) {
    for (int i = 0; i < 20; ++i) {
        beacon.announce(players, max_players);
        browser.poll();
        if (!browser.servers().empty()) return;
    }
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::printf("LAN discovery\n");

    Lsw2LanBrowser browser;
    if (!browser.start()) {
        /* A machine with the port already held, or no sockets at all. Exit 2 is
         * this project's "declined to pretend", the same code the master client
         * uses when no directory is running. */
        std::printf("  SKIP  could not bind the advertisement port\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }
    check(true, "the browser bound the advertisement port");

    Lsw2LanBeacon beacon;
    check(beacon.start("Maclunkey", 28550), "the beacon opened");
    check(beacon.running(), "and reports itself running");

    pump(beacon, browser, 1);
    check(browser.servers().size() == 1, "one lobby was heard");
    if (!browser.servers().empty()) {
        const Lsw2ServerEntry& e = browser.servers()[0];
        check(e.name == "Maclunkey", "with the host's name");
        check(e.port == 28550, "and the GAME's port, not the beacon's");
        check(e.players == 1 && e.max_players == 4, "and the player count");
        /* NOT A FIXED ADDRESS. The row is keyed by instance id and PREFERS the
         * routable address, so on a machine with a LAN interface this is the
         * LAN IP - the broadcast copy - and on an isolated one it is loopback.
         * Asserting either specifically would fail on the other kind of
         * machine, which is a test that reports the network rather than the
         * code. What must be true is that some usable address was recorded. */
        check(!e.address.empty(), "an address was taken from the datagram");
        std::printf("       (heard at %s)\n", e.address.c_str());
    }

    /* MERGING. A host announces every second; the row must update rather than
     * multiply. This is the bug that makes a browser useless after a minute. */
    for (int i = 0; i < 5; ++i) { beacon.announce(3, 4); browser.poll(); }
    check(browser.servers().size() == 1, "repeat announcements merge into one row");
    if (!browser.servers().empty())
        check(browser.servers()[0].players == 3, "and the count is updated");

    /* EXPIRY. A host that quits without saying so must fall out of the list. */
    beacon.stop();
    check(!beacon.running(), "the beacon stopped");
    /* DRAIN WHAT IT ALREADY SENT. Datagrams are delivered asynchronously, so a
     * copy announced just before the stop can still be in the browser's socket
     * and arrive during the "silence" below - refreshing the row and failing
     * the expiry, or re-adding the host after it and failing "heard once".
     * Measured 2026-09-25: the test failed one run in three on a quiet machine
     * with exactly those two checks. */
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        browser.poll();
    }
    /* THREE POLLS OF SILENCE against a limit of two. The unit is polls, not
     * seconds: the browser has no clock and counts poll() calls, and the caller
     * polls once per list refresh rather than once a frame. */
    for (int i = 0; i < 3; ++i) browser.poll();
    browser.expire(2);
    check(browser.servers().empty(), "a silent host expires from the list");

    /* HOSTILE INPUT. Anything on the subnet can send this, so the only rule is
     * that a bad datagram is ignored rather than believed. Sending them by hand
     * is the only way to cover the paths a well-behaved beacon never takes. */
    {
        Lsw2LanBeacon evil;
        check(evil.start("\x01\x02 bad\nname", 28551), "a hostile beacon opened");
        for (int i = 0; i < 20; ++i) { evil.announce(99, 99); browser.poll(); }
        check(browser.servers().size() == 1, "it was heard once");
        if (!browser.servers().empty()) {
            const Lsw2ServerEntry& e = browser.servers()[0];
            check(e.name == "badname", "its control characters were stripped");
            check(e.max_players == 4, "an absurd max is clamped");
            check(e.players <= e.max_players, "and players cannot exceed it");
        }
    }

    std::printf("a browser can ignore its own beacon\n");
    {
        /* A host broadcasts to the subnet and the subnet includes itself, so a
         * host running a browser would otherwise list its own lobby - and two
         * copies on one machine, which is how anyone tests this, would each
         * show two rows where there is one other game. */
        Lsw2LanBeacon mine;
        Lsw2LanBrowser view;
        check(mine.start("Maclunkey", 28560), "the beacon starts");
        check(mine.id() != 0, "it has an instance id");
        check(view.start(), "the browser starts");

        view.ignore(mine.id());
        pump(mine, view, 1, 4);
        bool listed_self = false;
        for (const Lsw2ServerEntry& e : view.servers())
            if (e.port == 28560) listed_self = true;
        check(!listed_self, "our own announcement is not in our own list");

        /* And a browser that ignores nothing DOES see it, so the filter is what
         * did it rather than the beacon failing to reach us. */
        Lsw2LanBrowser open_view;
        check(open_view.start(), "a second browser starts");
        pump(mine, open_view, 1, 4);
        bool seen = false;
        for (const Lsw2ServerEntry& e : open_view.servers())
            if (e.port == 28560) seen = true;
        check(seen, "a browser that ignores nothing does see it");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
#ifdef _WIN32
    WSACleanup();
#endif
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
