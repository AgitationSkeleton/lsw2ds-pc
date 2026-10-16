/* The relay SESSION wrapper: env/param -> port math, the link state machine
 * (host listen+accept, joiner non-blocking connect), and a real datagram
 * crossing through two sessions stood up in one process.
 *
 * lsw2_wireless_relay's own test already proves the byte crossing with the
 * relay objects driven directly; this proves that Lsw2RelaySession computes the
 * catch/inject ports the same way the python bridge does (catch = base+remote,
 * inject = base+local), establishes the TCP link both ways, and reaches Linked
 * - i.e. that the wrapper wires the proven relay correctly. */

#include "lsw2_relay_session.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define lsw2_close closesocket
#  define lsw2_sleep_ms(ms) Sleep(ms)
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define lsw2_close ::close
#  define lsw2_sleep_ms(ms) usleep((ms) * 1000)
#endif

static int g_checks = 0;
static int g_failed = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) std::printf("  ok: %s\n", what);
    else { ++g_failed; std::printf("  FAIL: %s\n", what); }
}

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

static socket_t udp_bind(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return s;
}

/* Wait up to ~2 s for a datagram, servicing both sessions as we poll. */
static int udp_recv_soon(socket_t s, void* buf, int cap,
                         Lsw2RelaySession& a, Lsw2RelaySession& b) {
    for (int i = 0; i < 200; ++i) {
        a.service();
        b.service();
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        timeval tv{0, 0};
        if (select(int(s) + 1, &rf, nullptr, nullptr, &tv) > 0) {
            const int n = int(recv(s, static_cast<char*>(buf), cap, 0));
            if (n > 0) return n;
        }
        lsw2_sleep_ms(10);
    }
    return -1;
}

int main() {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::printf("test_relay_session\n");

    const uint16_t link_port = 55231;
    /* Host: base 31000, seat 0, peer 1 -> catch 31001, inject 31000.
     * Joiner: base 41000, seat 1, peer 0 -> catch 41000, inject 41001. */
    Lsw2RelaySession host, join;
    check(host.start(true, link_port, "", 31000, 0, 1), "host session starts (listening)");
    check(host.phase() == Lsw2RelaySession::Phase::Listening, "host is listening");
    check(join.start(false, link_port, "127.0.0.1", 41000, 1, 0), "joiner session starts");

    /* Stand-ins for the two instances' listen sockets. */
    socket_t joiner_listen = udp_bind(41001);   /* host -> joiner lands here */
    socket_t host_listen   = udp_bind(31000);   /* joiner -> host lands here */

    /* Drive both until the link is up. */
    bool linked = false;
    for (int i = 0; i < 200 && !linked; ++i) {
        host.service();
        join.service();
        linked = host.phase() == Lsw2RelaySession::Phase::Linked &&
                 join.phase() == Lsw2RelaySession::Phase::Linked;
        lsw2_sleep_ms(10);
    }
    check(linked, "both sessions reach Linked over the loopback link");

    /* Host's instance sends a frame to its peer's slot (31001). It must land at
     * the joiner instance's listen port (41001), sender bytes intact. */
    const char* msg_a = "NIFI-from-host-to-joiner";
    udp_send(31001, msg_a, int(std::strlen(msg_a)));
    char got[256];
    int n = udp_recv_soon(joiner_listen, got, sizeof got, host, join);
    check(n == int(std::strlen(msg_a)) && std::memcmp(got, msg_a, n) == 0,
          "host->joiner datagram crosses (catch 31001 -> link -> inject 41001)");

    /* And the reverse: joiner (41000) -> host listen (31000). */
    const char* msg_b = "NIFI-from-joiner-to-host!";
    udp_send(41000, msg_b, int(std::strlen(msg_b)));
    n = udp_recv_soon(host_listen, got, sizeof got, host, join);
    check(n == int(std::strlen(msg_b)) && std::memcmp(got, msg_b, n) == 0,
          "joiner->host datagram crosses (catch 41000 -> link -> inject 31000)");

    Lsw2RelaySessionStatus hs = host.status();
    Lsw2RelaySessionStatus js = join.status();
    check(hs.forwarded >= 1 && js.injected >= 1, "host forwarded and joiner injected");
    check(js.forwarded >= 1 && hs.injected >= 1, "joiner forwarded and host injected");
    check(std::strcmp(hs.phase, "linked") == 0, "host status reads linked");

    lsw2_close(joiner_listen);
    lsw2_close(host_listen);
    host.stop();
    join.stop();

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
