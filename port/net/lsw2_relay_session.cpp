/* See lsw2_relay_session.h. */

#include "lsw2_relay_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define LSW2_INVALID INVALID_SOCKET
#  define lsw2_close closesocket
#  define lsw2_in_progress() (WSAGetLastError() == WSAEWOULDBLOCK || \
                              WSAGetLastError() == WSAEINPROGRESS)
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_in_progress() (errno == EINPROGRESS || errno == EAGAIN || \
                              errno == EWOULDBLOCK)
#endif

namespace {

constexpr uintptr_t kNone = ~uintptr_t(0);

socket_t sk(uintptr_t h) { return socket_t(h); }
bool have(uintptr_t h) { return h != kNone; }

void set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void set_nodelay(socket_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
               sizeof on);
}

/* One line per phase change, never per frame - a drive greps these, and a
 * normal run without the env never reaches here. */
void log_phase(const char* what, const char* detail) {
    std::fprintf(stderr, "[relay] %s%s%s\n", what, detail ? " " : "",
                 detail ? detail : "");
    std::fflush(stderr);
}

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    if (!e || !*e) return fallback;
    return std::atoi(e);
}

}  // namespace

Lsw2RelaySession::~Lsw2RelaySession() { stop(); }

bool Lsw2RelaySession::start_from_env() {
    const char* mode = std::getenv("LSW2_RELAY");
    if (!mode || !*mode) { phase_ = Phase::Off; return false; }
    const bool is_host = (std::strcmp(mode, "host") == 0);
    const bool is_connect = (std::strcmp(mode, "connect") == 0);
    if (!is_host && !is_connect) {
        log_phase("bad LSW2_RELAY (want host|connect):", mode);
        phase_ = Phase::Off;
        return false;
    }
    const uint16_t link_port = uint16_t(env_int("LSW2_RELAY_LINK_PORT", 55000));
    const int base   = env_int("LSW2_RELAY_BASE", 30000);
    const int local  = env_int("LSW2_RELAY_LOCAL", is_host ? 0 : 1);
    const int remote = env_int("LSW2_RELAY_REMOTE", is_host ? 1 : 0);
    const char* host_env = std::getenv("LSW2_RELAY_HOST");
    return start(is_host, link_port, (host_env && *host_env) ? host_env : "127.0.0.1",
                 base, local, remote);
}

bool Lsw2RelaySession::start(bool is_host, uint16_t link_port,
                             const std::string& host_addr,
                             int base, int local, int remote) {
    stop();
    phase_ = Phase::Off;
    is_host_ = is_host;
    link_port_ = link_port;
    host_addr_ = host_addr;
    catch_port_  = uint16_t(base + remote);   /* frames this instance sends the peer */
    inject_port_ = uint16_t(base + local);    /* the port this instance listens on */

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);          /* refcounted; harmless if repeated */
#endif
    char d[192];
    std::snprintf(d, sizeof d,
                  "link:%u base:%d local:%d remote:%d catch:%u inject:%u",
                  link_port_, base, local, remote, catch_port_, inject_port_);
    return is_host_ ? begin_listen(d) : begin_connect(d);
}

void Lsw2RelaySession::service() {
    switch (phase_) {
        case Phase::Off:
        case Phase::Closed:
            return;
        case Phase::Listening:  poll_accept();  return;
        case Phase::Connecting: poll_connect(); return;
        case Phase::Linked:
            /* The pump thread does the byte moving; here we only notice it
             * reporting the link gone. */
            if (link_closed_.load(std::memory_order_relaxed)) {
                log_phase("link closed; relay stopped", nullptr);
                stop();
                phase_ = Phase::Closed;
            }
            return;
    }
}

void Lsw2RelaySession::pump_loop() {
    /* Continuous, independent of the frame loop, so it keeps injecting the
     * peer's frames even while the guest's WH wait blocks that loop. The relay
     * sockets are non-blocking; a short sleep keeps this off a spin. */
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!relay_.service()) {
            link_closed_.store(true, std::memory_order_relaxed);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void Lsw2RelaySession::join_pump() {
    stop_.store(true, std::memory_order_relaxed);
    if (pump_.joinable()) pump_.join();
}

void Lsw2RelaySession::stop() {
    join_pump();          /* stop the pump BEFORE closing the sockets it uses */
    relay_.stop();
    if (have(link_))   { lsw2_close(sk(link_));   link_ = kNone; }
    if (have(listen_)) { lsw2_close(sk(listen_)); listen_ = kNone; }
    if (phase_ != Phase::Off) phase_ = Phase::Closed;
}

Lsw2RelaySessionStatus Lsw2RelaySession::status() const {
    const char* p = "off";
    switch (phase_) {
        case Phase::Off:        p = "off";        break;
        case Phase::Listening:  p = "listening";  break;
        case Phase::Connecting: p = "connecting"; break;
        case Phase::Linked:     p = "linked";     break;
        case Phase::Closed:     p = "closed";     break;
    }
    return { p, relay_.forwarded(), relay_.injected() };
}

bool Lsw2RelaySession::begin_listen(const char* d) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == LSW2_INVALID) { log_phase("listen socket failed", d); return false; }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
               sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(link_port_);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        listen(s, 1) != 0) {
        lsw2_close(s);
        log_phase("listen/bind failed", d);
        return false;
    }
    set_nonblocking(s);
    listen_ = uintptr_t(s);
    phase_ = Phase::Listening;
    log_phase("hosting, awaiting joiner link;", d);
    return true;
}

void Lsw2RelaySession::poll_accept() {
    socket_t c = accept(sk(listen_), nullptr, nullptr);
    if (c == LSW2_INVALID) return;             /* no joiner yet */
    lsw2_close(sk(listen_));
    listen_ = kNone;
    set_nodelay(c);
    set_nonblocking(c);
    link_ = uintptr_t(c);
    arm_relay();
}

bool Lsw2RelaySession::begin_connect(const char* d) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == LSW2_INVALID) { log_phase("connect socket failed", d); return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(link_port_);
    if (inet_pton(AF_INET, host_addr_.c_str(), &a.sin_addr) != 1) {
        lsw2_close(s);
        log_phase("bad LSW2_RELAY_HOST", host_addr_.c_str());
        return false;
    }
    set_nonblocking(s);
    const int r = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    link_ = uintptr_t(s);
    if (r == 0) {                              /* immediate (loopback often is) */
        set_nodelay(s);
        arm_relay();
        return true;
    }
    if (!lsw2_in_progress()) {
        lsw2_close(s);
        link_ = kNone;
        log_phase("connect failed", d);
        phase_ = Phase::Closed;
        return false;
    }
    phase_ = Phase::Connecting;
    log_phase("connecting to host link;", d);
    return true;
}

void Lsw2RelaySession::poll_connect() {
    fd_set wf, ef;
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(sk(link_), &wf); FD_SET(sk(link_), &ef);
    timeval tv{0, 0};
    const int n = select(int(sk(link_)) + 1, nullptr, &wf, &ef, &tv);
    if (n <= 0) return;                        /* still connecting */
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(sk(link_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (FD_ISSET(sk(link_), &ef) || err != 0) {
        lsw2_close(sk(link_));
        link_ = kNone;
        log_phase("connect refused/failed", nullptr);
        phase_ = Phase::Closed;
        return;
    }
    set_nodelay(sk(link_));
    arm_relay();
}

void Lsw2RelaySession::arm_relay() {
    if (!relay_.start(catch_port_, inject_port_, link_)) {
        log_phase("relay bind failed (catch/inject port in use?)", nullptr);
        stop();
        phase_ = Phase::Closed;
        return;
    }
    stop_.store(false, std::memory_order_relaxed);
    link_closed_.store(false, std::memory_order_relaxed);
    pump_ = std::thread([this] { pump_loop(); });
    phase_ = Phase::Linked;
    log_phase("linked; carrying the DS radio (pump thread up)", nullptr);
}

/* ---- the singleton the shell drives ---------------------------------- */

namespace {
std::unique_ptr<Lsw2RelaySession> g_session;
bool g_started = false;
}  // namespace

bool lsw2_relay_session_start_from_env() {
    if (g_started)
        return g_session && g_session->phase() != Lsw2RelaySession::Phase::Off;
    g_started = true;
    g_session = std::make_unique<Lsw2RelaySession>();
    if (!g_session->start_from_env()) { g_session.reset(); return false; }
    return true;
}

void lsw2_relay_session_service() {
    if (g_session) g_session->service();
}

void lsw2_relay_session_stop() {
    if (g_session) { g_session->stop(); g_session.reset(); }
}

Lsw2RelaySessionStatus lsw2_relay_session_status() {
    if (g_session) return g_session->status();
    return { "off", 0, 0 };
}

bool lsw2_relay_session_start_explicit(bool is_host, uint16_t link_port,
                                       const char* host_addr, int base,
                                       int local, int remote) {
    /* The env path wins if it already stood a relay up (LSW2_RELAY set): leave
     * that one alone rather than tearing down a live link. */
    if (g_started && g_session &&
        g_session->phase() != Lsw2RelaySession::Phase::Off)
        return true;
    /* Already brought up explicitly (a second host/join in the same run without a
     * teardown, or the service loop calling again): don't re-bind a live link. */
    if (g_session && g_session->phase() != Lsw2RelaySession::Phase::Off)
        return true;
    g_started = true;
    if (!g_session) g_session = std::make_unique<Lsw2RelaySession>();
    if (!g_session->start(is_host, link_port,
                          (host_addr && *host_addr) ? host_addr : "127.0.0.1",
                          base, local, remote)) {
        g_session.reset();
        return false;
    }
    return true;
}
