/* The lobby-host ping probe.
 *
 *   g++ -O2 -std=c++20 port/net/test_ping.cpp port/net/lsw2_ping.cpp \
 *       -o test_ping -lws2_32
 *
 * Against a real listener on loopback rather than a mock, for the reason the
 * master-client test gives: what a probe measures is the behaviour of a socket,
 * and a mock would agree with whatever this file believed.
 */

#include "lsw2_ping.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define LSW2_INVALID INVALID_SOCKET
#  define lsw2_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#endif

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/* A listener that accepts nothing. The connect still completes - the kernel's
 * backlog does that - which is exactly what a busy host looks like. */
socket_t open_listener(int* port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == LSW2_INVALID) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::listen(s, 8) != 0) {
        lsw2_close(s);
        return LSW2_INVALID;
    }
#ifdef _WIN32
    int len = sizeof a;
#else
    socklen_t len = sizeof a;
#endif
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    *port = int(ntohs(a.sin_port));
    return s;
}

/* A port nothing is listening on. Bind one, read its number, close it. */
int dead_port() {
    int port = 0;
    socket_t s = open_listener(&port);
    if (s != LSW2_INVALID) lsw2_close(s);
    return port;
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int port = 0;
    socket_t listener = open_listener(&port);
    if (listener == LSW2_INVALID) {
        std::printf("  SKIP  could not open a loopback listener\n");
        return 2;
    }
    std::printf("a host that is listening answers\n");

    const int one = lsw2_ping_one("127.0.0.1", port, 1000);
    check("a live host answers", one >= 0);
    check("and a loopback answer is fast", one >= 0 && one < 500);

    std::printf("a host that is not listening does not\n");
    const int dead = dead_port();
    const int refused = lsw2_ping_one("127.0.0.1", dead, 1000);
    /* THIS IS THE CHECK THAT EARNS THE SO_ERROR READ. A refused connection
     * selects WRITABLE just as a successful one does, so without inspecting
     * SO_ERROR a closed port would report the fastest ping in the list - the
     * most confident possible way to be wrong. */
    check("a refused port is -1, not a very fast ping", refused == -1);

    std::printf("nonsense targets are refused rather than probed\n");
    check("an empty address", lsw2_ping_one("", port, 500) == -1);
    check("port zero", lsw2_ping_one("127.0.0.1", 0, 500) == -1);
    check("a port out of range", lsw2_ping_one("127.0.0.1", 99999, 500) == -1);
    check("a hostname - there is no resolver here, by design",
          lsw2_ping_one("localhost", port, 500) == -1);
    check("a zero budget probes nothing",
          lsw2_ping_one("127.0.0.1", port, 0) == -1);

    std::printf("a sweep answers the reachable and leaves the rest alone\n");
    {
        std::vector<Lsw2PingTarget> targets(5);
        targets[0].address = "127.0.0.1"; targets[0].port = port;
        targets[1].address = "127.0.0.1"; targets[1].port = dead;
        targets[2].address = "127.0.0.1"; targets[2].port = port;
        targets[3].address = "";          targets[3].port = port;
        /* TEST-NET-1, which is not routed anywhere - so it never answers and
         * the sweep has to give up on it rather than wait for it. */
        targets[4].address = "192.0.2.1"; targets[4].port = 28550;

        const int answered = lsw2_ping_sweep(targets, 1200);
        check("the two live hosts answered", answered == 2);
        check("and they are the ones that did",
              targets[0].ping_ms >= 0 && targets[2].ping_ms >= 0);
        check("the refused one is -1", targets[1].ping_ms == -1);
        check("the empty one is -1", targets[3].ping_ms == -1);
        check("the unroutable one is -1", targets[4].ping_ms == -1);
    }

    std::printf("the budget bounds the SWEEP, not each target\n");
    {
        /* Eight unroutable hosts. Serially at the budget each, this would take
         * eight times as long; the whole point of one select over all of them is
         * that it does not. */
        std::vector<Lsw2PingTarget> targets(8);
        for (auto& t : targets) { t.address = "192.0.2.1"; t.port = 28550; }

        const auto began = std::chrono::steady_clock::now();
        lsw2_ping_sweep(targets, 400);
        const int spent = int(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began).count());

        check("none answered", [&] {
            for (const auto& t : targets) if (t.ping_ms >= 0) return false;
            return true;
        }());
        check("and eight of them cost one budget, not eight", spent < 1200);
    }

    lsw2_close(listener);
    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
