/* The wireless relay: the framing (pure) and a real loopback crossing.
 *
 * Mirrors tools/wireless_bridge.py's selftest: two relays over a loopback TCP
 * pair, two mock instances on DIFFERENT bases, a datagram that must cross each
 * way with its exact bytes, and a frame too big for one recv still whole. */

#include "lsw2_wireless_relay.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define lsw2_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define lsw2_close ::close
#endif

static int g_checks = 0;
static int g_failed = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) std::printf("  ok: %s\n", what);
    else { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

/* Send a UDP datagram to a loopback port. */
static void udp_send(uint16_t port, const void* data, int len) {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    sendto(s, static_cast<const char*>(data), len, 0,
           reinterpret_cast<sockaddr*>(&a), sizeof a);
    lsw2_close(s);
}

/* A UDP socket bound to a loopback port, to stand in for an instance's listener. */
static socket_t udp_bind(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
#ifdef _WIN32
    u_long on = 1; ioctlsocket(s, FIONBIO, &on);
#endif
    return s;
}

/* Pump both relays a few times to let bytes cross. */
static void pump(Lsw2WirelessRelay& a, Lsw2WirelessRelay& b, int times) {
    for (int i = 0; i < times; ++i) { a.service(); b.service(); }
}

/* Read one datagram from a bound UDP socket, or empty on nothing (after pumps). */
static std::vector<uint8_t> udp_try_recv(socket_t s) {
    char buf[4096];
    for (int tries = 0; tries < 50; ++tries) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n > 0) return std::vector<uint8_t>(buf, buf + n);
#ifndef _WIN32
        (void)0;
#endif
    }
    return {};
}

int main() {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* --- the pure framing, first --- */
    {
        std::vector<uint8_t> wire;
        const uint8_t p[3] = {0xAA, 0xBB, 0xCC};
        Lsw2WirelessRelay::frame(p, 3, wire);
        check(wire.size() == 7 && wire[0] == 3 && wire[4] == 0xAA && wire[6] == 0xCC,
              "frame() prefixes a 4-byte LE length then the bytes");

        /* two frames back to back deframe into two, exact bytes */
        std::vector<uint8_t> two;
        Lsw2WirelessRelay::frame(p, 3, two);
        const uint8_t q[2] = {0x11, 0x22};
        Lsw2WirelessRelay::frame(q, 2, two);
        std::vector<std::vector<uint8_t>> got;
        const int n = Lsw2WirelessRelay::deframe(two, got);
        check(n == 2 && got[0].size() == 3 && got[1].size() == 2 && got[1][1] == 0x22,
              "deframe() splits two concatenated frames exactly");
        check(two.empty(), "deframe() consumes what it delivered");

        /* a partial trailing frame is kept for next time */
        std::vector<uint8_t> partial;
        Lsw2WirelessRelay::frame(p, 3, partial);
        partial.push_back(10); partial.push_back(0); partial.push_back(0); partial.push_back(0);
        partial.push_back(0xEE);   /* says 10 bytes, only 1 present */
        got.clear();
        const int m = Lsw2WirelessRelay::deframe(partial, got);
        check(m == 1 && partial.size() == 5, "deframe() leaves a partial frame buffered");
    }

    /* --- a real crossing over loopback, each way --- */
    const uint16_t base_a = 47100, base_b = 48100;
    const int local_a = 0, remote_a = 1;   /* machine A is instance 0 */
    const int local_b = 1, remote_b = 0;   /* machine B is instance 1 */

    socket_t inst_a = udp_bind(uint16_t(base_a + local_a));   /* A listens A+0 */
    socket_t inst_b = udp_bind(uint16_t(base_b + local_b));   /* B listens B+1 */

    /* the TCP link between the two bridges */
    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    listen(srv, 1);
    socklen_t slen = sizeof sa; getsockname(srv, reinterpret_cast<sockaddr*>(&sa), &slen);
    const uint16_t link_port = ntohs(sa.sin_port);

    socket_t link_b = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in ca{}; ca.sin_family = AF_INET; ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ca.sin_port = htons(link_port);
    connect(link_b, reinterpret_cast<sockaddr*>(&ca), sizeof ca);
    socket_t link_a = accept(srv, nullptr, nullptr);
#ifdef _WIN32
    u_long on = 1; ioctlsocket(link_a, FIONBIO, &on); ioctlsocket(link_b, FIONBIO, &on);
#endif

    Lsw2WirelessRelay relay_a, relay_b;
    /* A catches A+remote (frames for the peer), injects A+local */
    check(relay_a.start(uint16_t(base_a + remote_a), uint16_t(base_a + local_a), uintptr_t(link_a)),
          "relay A binds its catch/inject sockets");
    check(relay_b.start(uint16_t(base_b + remote_b), uint16_t(base_b + local_b), uintptr_t(link_b)),
          "relay B binds its catch/inject sockets");

    /* A's instance sends a frame to peer 1 -> A catches A+1 -> link -> B injects B+1 */
    const char* msg_ab = "NIFI-A-to-B-payload";
    udp_send(uint16_t(base_a + remote_a), msg_ab, int(std::strlen(msg_ab)));
    pump(relay_a, relay_b, 20);
    std::vector<uint8_t> at_b = udp_try_recv(inst_b);
    check(!at_b.empty() && std::memcmp(at_b.data(), msg_ab, at_b.size()) == 0,
          "a frame from instance 0 reaches instance 1 across the relay, exact bytes");

    /* the reverse */
    const char* msg_ba = "NIFI-B-back-to-A";
    udp_send(uint16_t(base_b + remote_b), msg_ba, int(std::strlen(msg_ba)));
    pump(relay_a, relay_b, 20);
    std::vector<uint8_t> at_a = udp_try_recv(inst_a);
    check(!at_a.empty() && std::memcmp(at_a.data(), msg_ba, at_a.size()) == 0,
          "a frame from instance 1 reaches instance 0 across the relay");

    /* a frame bigger than one TCP recv chunk still arrives whole */
    std::vector<uint8_t> big(2500, 0x5A);
    udp_send(uint16_t(base_a + remote_a), big.data(), int(big.size()));
    pump(relay_a, relay_b, 40);
    std::vector<uint8_t> big_at_b = udp_try_recv(inst_b);
    check(big_at_b.size() == big.size(), "a 2500-byte frame crosses whole (link framing holds)");

    check(relay_a.forwarded() >= 2 && relay_b.injected() >= 2,
          "the relay counters tally the crossings");

    relay_a.stop();
    relay_b.stop();
    lsw2_close(link_a); lsw2_close(link_b); lsw2_close(srv);
    lsw2_close(inst_a); lsw2_close(inst_b);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
