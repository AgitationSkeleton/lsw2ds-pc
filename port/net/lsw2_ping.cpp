/* See lsw2_ping.h. */

#include "lsw2_ping.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define LSW2_INVALID INVALID_SOCKET
#  define lsw2_close closesocket
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#endif

namespace {

using Clock = std::chrono::steady_clock;

int elapsed_ms(Clock::time_point from) {
    const auto d = Clock::now() - from;
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

void set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool connect_in_progress() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

/* One probe in flight. */
struct Probe {
    socket_t fd = LSW2_INVALID;
    int index = -1;
    Clock::time_point started;
    bool done = false;
};

}  // namespace

int lsw2_ping_sweep(std::vector<Lsw2PingTarget>& targets, int budget_ms) {
    if (targets.empty() || budget_ms <= 0) return 0;

    std::vector<Probe> probes;
    probes.reserve(targets.size());

    for (size_t i = 0; i < targets.size(); ++i) {
        Lsw2PingTarget& t = targets[i];
        t.ping_ms = -1;
        if (t.address.empty() || t.port <= 0 || t.port > 65535) continue;

        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(uint16_t(t.port));
        if (::inet_pton(AF_INET, t.address.c_str(), &to.sin_addr) != 1) {
            /* NO NAME RESOLUTION HERE. The directory hands out addresses it
             * took from a socket, so a name would be something a host invented -
             * and resolving one would block this sweep on DNS, which is the
             * cost this design exists to avoid. */
            continue;
        }

        socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == LSW2_INVALID) continue;
        set_nonblocking(s);

        Probe p;
        p.fd = s;
        p.index = int(i);
        p.started = Clock::now();

        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
            /* Connected immediately - a loopback host, usually. */
            t.ping_ms = elapsed_ms(p.started);
            lsw2_close(s);
            continue;
        }
        if (!connect_in_progress()) {
            lsw2_close(s);
            continue;
        }
        probes.push_back(p);
    }

    const auto sweep_started = Clock::now();
    int answered = 0;
    for (const Lsw2PingTarget& t : targets)
        if (t.ping_ms >= 0) ++answered;

    while (true) {
        const int spent = elapsed_ms(sweep_started);
        if (spent >= budget_ms) break;

        fd_set writable;
        FD_ZERO(&writable);
        socket_t highest = 0;
        int waiting = 0;
        for (const Probe& p : probes) {
            if (p.done) continue;
            FD_SET(p.fd, &writable);
            if (p.fd > highest) highest = p.fd;
            ++waiting;
        }
        if (waiting == 0) break;

        const int left = budget_ms - spent;
        timeval tv{};
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;

        const int ready = ::select(int(highest) + 1, nullptr, &writable, nullptr,
                                   &tv);
        if (ready <= 0) break;              /* timed out, or the wait failed */

        for (Probe& p : probes) {
            if (p.done || !FD_ISSET(p.fd, &writable)) continue;
            p.done = true;

            /* WRITABLE IS NOT CONNECTED. A refused connection also selects
             * writable, and reading SO_ERROR is the only way to tell the two
             * apart - without it a closed port would report a very fast ping,
             * which is the most confident possible way to be wrong. */
            int err = 0;
#ifdef _WIN32
            int len = sizeof err;
            ::getsockopt(p.fd, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&err), &len);
#else
            socklen_t len = sizeof err;
            ::getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
            if (err == 0) {
                targets[size_t(p.index)].ping_ms = elapsed_ms(p.started);
                ++answered;
            }
            lsw2_close(p.fd);
            p.fd = LSW2_INVALID;
        }
    }

    for (Probe& p : probes)
        if (p.fd != LSW2_INVALID) lsw2_close(p.fd);

    return answered;
}

int lsw2_ping_one(const std::string& address, int port, int budget_ms) {
    std::vector<Lsw2PingTarget> one(1);
    one[0].address = address;
    one[0].port = port;
    lsw2_ping_sweep(one, budget_ms);
    return one[0].ping_ms;
}
