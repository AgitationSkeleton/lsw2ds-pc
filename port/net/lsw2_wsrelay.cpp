/* See lsw2_wsrelay.h. */

#include "lsw2_wsrelay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

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
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#endif

namespace {

using Clock = std::chrono::steady_clock;

/* How often an idle connection says something, so neither the relay nor a
 * proxy in between closes it for silence (Cloudflare drops a WebSocket idle
 * for about 100 s). */
constexpr int kKeepaliveMs = 20000;
constexpr int kRecvChunk = 16384;

long long as_fd(socket_t s) { return s == LSW2_INVALID ? -1 : (long long)s; }
socket_t as_sock(long long fd) { return fd < 0 ? LSW2_INVALID : socket_t(fd); }

int ms_since(Clock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

void set_blocking(socket_t s, bool blocking) {
#ifdef _WIN32
    u_long on = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    const int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
#endif
}

void set_nodelay(socket_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

/* A send that cannot wedge the bridge forever on a peer that stopped reading. */
void set_send_timeout(socket_t s, int ms) {
#ifdef _WIN32
    DWORD t = DWORD(ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

bool send_all(socket_t s, const char* p, size_t n) {
    while (n > 0) {
        const int k = ::send(s, p, int(std::min<size_t>(n, 1u << 20)), 0);
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
}

/* Wait until `s` is readable or `ms` passes. */
bool wait_readable(socket_t s, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(int(s) + 1, &rd, nullptr, nullptr, &tv) > 0;
}

/* Resolve and connect within `ms`, leaving the socket BLOCKING. */
socket_t connect_within(const std::string& host, int port, int ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portbuf[16];
    std::snprintf(portbuf, sizeof portbuf, "%d", port);
    if (getaddrinfo(host.c_str(), portbuf, &hints, &res) != 0 || !res) return LSW2_INVALID;
    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == LSW2_INVALID) { freeaddrinfo(res); return LSW2_INVALID; }
    set_blocking(s, false);
    bool ok = ::connect(s, res->ai_addr, int(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) {
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        if (::select(int(s) + 1, nullptr, &wr, &ex, &tv) > 0 && !FD_ISSET(s, &ex)) {
            int err = 0;
            socklen_t len = sizeof err;
            ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0;
        }
    }
    if (!ok) { lsw2_close(s); return LSW2_INVALID; }
    set_blocking(s, true);
    set_nodelay(s);
    set_send_timeout(s, 5000);
    return s;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* k =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) |
                           (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0u) |
                           (i + 2 < n ? uint32_t(p[i + 2]) : 0u);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

uint32_t next_rand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

/* A close frame's reason, for the player: the room says why it refused. */
std::string close_text(const std::string& payload) {
    return payload.size() > 2 ? payload.substr(2) : std::string();
}

}  // namespace

/* ------------------------------------------------------------- the framing */

std::string lsw2_ws_frame(uint8_t opcode, const uint8_t* data, size_t n,
                          uint32_t mask) {
    std::string f;
    f.reserve(n + 14);
    f.push_back(char(0x80u | (opcode & 0x0Fu)));
    if (n < 126) {
        f.push_back(char(0x80u | n));
    } else if (n < 65536) {
        f.push_back(char(0x80u | 126u));
        f.push_back(char((n >> 8) & 0xFFu));
        f.push_back(char(n & 0xFFu));
    } else {
        f.push_back(char(0x80u | 127u));
        for (int i = 7; i >= 0; --i) f.push_back(char((uint64_t(n) >> (8 * i)) & 0xFFu));
    }
    const uint8_t mk[4] = {uint8_t(mask >> 24), uint8_t(mask >> 16),
                           uint8_t(mask >> 8), uint8_t(mask)};
    f.append((const char*)mk, 4);
    for (size_t i = 0; i < n; ++i) f.push_back(char(data[i] ^ mk[i & 3]));
    return f;
}

bool lsw2_ws_parse(std::string& buf, std::string& partial, uint8_t& partial_op,
                   std::vector<Lsw2WsMessage>* out) {
    size_t pos = 0;
    for (;;) {
        const size_t avail = buf.size() - pos;
        if (avail < 2) break;
        const uint8_t b0 = uint8_t(buf[pos]), b1 = uint8_t(buf[pos + 1]);
        const bool fin = (b0 & 0x80u) != 0;
        const uint8_t op = b0 & 0x0Fu;
        const bool masked = (b1 & 0x80u) != 0;
        uint64_t len = b1 & 0x7Fu;
        size_t h = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = (uint64_t(uint8_t(buf[pos + 2])) << 8) | uint8_t(buf[pos + 3]);
            h = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(buf[pos + 2 + size_t(i)]);
            h = 10;
        }
        if (len > (1u << 24)) return false;      /* nothing here is that big */
        uint8_t mk[4] = {0, 0, 0, 0};
        if (masked) {
            if (avail < h + 4) break;
            std::memcpy(mk, buf.data() + pos + h, 4);
            h += 4;
        }
        if (avail < h + len) break;
        std::string payload = buf.substr(pos + h, size_t(len));
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mk[i & 3]);
        pos += h + size_t(len);

        if (op >= 8) {                           /* control frames are whole */
            out->push_back({op, std::move(payload)});
        } else if (op == 0) {                    /* a continuation */
            partial += payload;
            if (fin) {
                out->push_back({partial_op, partial});
                partial.clear();
                partial_op = 0;
            }
        } else if (fin) {
            out->push_back({op, std::move(payload)});
        } else {
            partial_op = op;
            partial = std::move(payload);
        }
    }
    buf.erase(0, pos);
    return true;
}

/* ------------------------------------------------------------- the client */

bool Lsw2WsClient::open(const std::string& host, int port, const std::string& path,
                        const std::string& token, int timeout_ms, std::string* error) {
    close();
    const auto t0 = Clock::now();
    socket_t s = connect_within(host, port, timeout_ms);
    if (s == LSW2_INVALID) {
        if (error) *error = "cannot reach the relay at " + host;
        return false;
    }
    rng_ ^= uint32_t(Clock::now().time_since_epoch().count()) | 1u;
    uint8_t key[16];
    for (uint8_t& b : key) b = uint8_t(next_rand(rng_));
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + (port == 80 ? std::string() : ":" + std::to_string(port)) + "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + base64(key, sizeof key) + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    if (!token.empty()) req += "X-Token: " + token + "\r\n";
    req += "\r\n";
    if (!send_all(s, req.data(), req.size())) {
        lsw2_close(s);
        if (error) *error = "the relay closed the connection";
        return false;
    }

    std::string head;
    size_t end = std::string::npos;
    while (end == std::string::npos) {
        const int left = timeout_ms - ms_since(t0);
        if (left <= 0 || !wait_readable(s, left)) {
            lsw2_close(s);
            if (error) *error = "the relay did not answer";
            return false;
        }
        char tmp[4096];
        const int k = ::recv(s, tmp, int(sizeof tmp), 0);
        if (k <= 0) {
            lsw2_close(s);
            if (error) *error = "the relay closed the connection";
            return false;
        }
        head.append(tmp, size_t(k));
        end = head.find("\r\n\r\n");
        if (head.size() > 65536) break;
    }
    if (end == std::string::npos || head.compare(0, 12, "HTTP/1.1 101") != 0) {
        lsw2_close(s);
        if (error) {
            /* The status line and, for an error, the JSON body's message. */
            const std::string status = head.substr(0, head.find("\r\n"));
            if (status.find(" 404") != std::string::npos)
                *error = "that lobby is no longer open";
            else if (status.find(" 401") != std::string::npos)
                *error = "the relay did not accept this lobby's token";
            else if (status.find(" 503") != std::string::npos ||
                     status.find(" 426") != std::string::npos)
                *error = "the master server has no relay";
            else
                *error = "the relay refused (" + status + ")";
        }
        return false;
    }
    buf_ = head.substr(end + 4);    /* the first frames may ride with the header */
    partial_.clear();
    partial_op_ = 0;
    early_.clear();
    fd_ = as_fd(s);
    if (!lsw2_ws_parse(buf_, partial_, partial_op_, &early_)) {
        close();
        if (error) *error = "the relay sent a malformed frame";
        return false;
    }
    return true;
}

bool Lsw2WsClient::send_frame(uint8_t opcode, const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> hold(send_mu_);
    const long long fd = fd_.load();
    if (fd < 0) return false;
    const std::string f = lsw2_ws_frame(opcode, data, n, next_rand(rng_));
    if (!send_all(as_sock(fd), f.data(), f.size())) {
        /* Closed here, under the lock already held - close() would take it. */
        long long expect = fd;
        if (fd_.compare_exchange_strong(expect, -1)) lsw2_close(as_sock(fd));
        return false;
    }
    return true;
}

bool Lsw2WsClient::send_binary(const uint8_t* data, size_t n) { return send_frame(2, data, n); }

bool Lsw2WsClient::send_text(const std::string& text) {
    return send_frame(1, (const uint8_t*)text.data(), text.size());
}

bool Lsw2WsClient::receive(std::vector<Lsw2WsMessage>* out) {
    const long long fd = fd_.load();
    if (fd < 0) return false;
    char tmp[kRecvChunk];
    const int k = ::recv(as_sock(fd), tmp, int(sizeof tmp), 0);
    if (k <= 0) { close(); return false; }
    buf_.append(tmp, size_t(k));
    std::vector<Lsw2WsMessage> got;
    if (!lsw2_ws_parse(buf_, partial_, partial_op_, &got)) { close(); return false; }
    bool open = true;
    for (Lsw2WsMessage& m : got) {
        if (m.opcode == 9) {                          /* ping -> pong */
            send_frame(10, (const uint8_t*)m.data.data(), m.data.size());
            continue;
        }
        if (m.opcode == 10) continue;
        if (m.opcode == 8) {
            open = false;
            close_reason_ = close_text(m.data);
        }
        out->push_back(std::move(m));
    }
    if (!open) {
        send_frame(8, nullptr, 0);
        close();
    }
    return open;
}

void Lsw2WsClient::close() {
    std::lock_guard<std::mutex> hold(send_mu_);
    const long long fd = fd_.exchange(-1);
    if (fd >= 0) lsw2_close(as_sock(fd));
}

/* ------------------------------------------------------------- the stream */

std::string Lsw2LaneStream::wrap(const char* data, size_t n) {
    std::string out(4, '\0');
    const uint32_t seq = send_seq_++;
    for (int i = 0; i < 4; ++i) out[size_t(i)] = char((seq >> (8 * i)) & 0xFFu);
    out.append(data, n);
    return out;
}

bool Lsw2LaneStream::take(const std::string& chunk, std::string* out) {
    if (chunk.size() < 4) return false;
    uint32_t seq = 0;
    for (int i = 0; i < 4; ++i) seq |= uint32_t(uint8_t(chunk[size_t(i)])) << (8 * i);
    if (seq < next_) return true;                       /* a copy: already delivered */
    if (seq > next_) {
        /* Ahead of a gap another lane will fill. Bounded: a lane that far ahead
         * means the stream is broken, not late. */
        if (early_.size() > 4096) return false;
        early_.emplace(seq, chunk.substr(4));
        return true;
    }
    out->append(chunk, 4, std::string::npos);
    ++next_;
    for (auto it = early_.find(next_); it != early_.end(); it = early_.find(next_)) {
        out->append(it->second);
        early_.erase(it);
        ++next_;
    }
    return true;
}

int lsw2_relay_lanes() {
    static const int n = [] {
        const char* v = std::getenv("LSW2_RELAY_LANES");
        const int k = v ? std::atoi(v) : 2;
        return k < 1 ? 1 : (k > 4 ? 4 : k);
    }();
    return n;
}

namespace {

/* TEST ONLY, on a joiner's lanes - a long-distance and lossy link reproduced
 * on one machine:
 *   LSW2_TEST_NET_DELAY_MS=<ms>         every chunk held that long, each way
 *   LSW2_TEST_LANE_STALL=<ms>,<per1000> each chunk has that chance of freezing
 *                                       its lane for <ms> - what a lost packet
 *                                       does to a TCP stream
 * Unset, none of this runs. */
struct TestLink {
    int delay_ms = 0;
    int stall_ms = 0;
    int stall_per_mille = 0;
    bool on() const { return delay_ms > 0 || stall_per_mille > 0; }
};

const TestLink& test_link() {
    static const TestLink t = [] {
        TestLink x;
        if (const char* v = std::getenv("LSW2_TEST_NET_DELAY_MS")) x.delay_ms = std::max(0, std::atoi(v));
        if (const char* v = std::getenv("LSW2_TEST_LANE_STALL"))
            std::sscanf(v, "%d,%d", &x.stall_ms, &x.stall_per_mille);
        return x;
    }();
    return t;
}

struct Held {
    Clock::time_point due;
    std::string bytes;
};

/* One lane's test queues: what it will send and what it has received, each
 * released in order no earlier than its due time. */
struct TestLane {
    std::mutex mu;
    std::deque<Held> up, down;
    Clock::time_point frozen_until{};
    Clock::time_point last_up{}, last_down{};
    uint32_t rng = 0x12345u;

    Clock::time_point due_now(Clock::time_point& last) {
        const TestLink& t = test_link();
        const auto now = Clock::now();
        if (t.stall_per_mille > 0) {
            rng = rng * 1103515245u + 12345u;
            if (int((rng >> 8) % 1000u) < t.stall_per_mille)
                frozen_until = std::max(frozen_until, now + std::chrono::milliseconds(t.stall_ms));
        }
        auto due = now + std::chrono::milliseconds(t.delay_ms);
        if (due < frozen_until) due = frozen_until;
        if (due < last) due = last;                    /* a lane stays in order */
        last = due;
        return due;
    }
};

TestLane g_test_lanes[4];

}  // namespace

/* ------------------------------------------------------------- the host */

struct Lsw2RelayHost::Peer {
    socket_t sock = LSW2_INVALID;
    Lsw2LaneStream stream;
};

Lsw2RelayHost::~Lsw2RelayHost() { stop(); }

Lsw2RelayHost::Lsw2RelayHost(std::string master_host, int master_port, std::string id,
                             std::string token, uint16_t listen_port)
    : host_(std::move(master_host)), id_(std::move(id)), token_(std::move(token)),
      port_(master_port), listen_port_(listen_port) {}

void Lsw2RelayHost::start() {
    if (!threads_.empty() || id_.empty() || token_.empty()) return;
    stop_ = false;
    lanes_n_ = lsw2_relay_lanes();
    lanes_.clear();
    for (int k = 0; k < lanes_n_; ++k) lanes_.push_back(std::make_unique<Lsw2WsClient>());
    for (int k = 0; k < lanes_n_; ++k) threads_.emplace_back([this, k] { lane_run(k); });
    threads_.emplace_back([this] { local_run(); });
}

void Lsw2RelayHost::stop() {
    stop_ = true;
    for (auto& l : lanes_) if (l) l->close();
    for (std::thread& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
    std::lock_guard<std::mutex> hold(mu_);
    for (auto& p : peers_map_) if (p.second->sock != LSW2_INVALID) lsw2_close(p.second->sock);
    peers_map_.clear();
    lanes_up_ = 0;
    peers_ = 0;
}

void Lsw2RelayHost::send_all_lanes(const std::string& msg) {
    for (auto& l : lanes_)
        if (l->is_open()) l->send_binary((const uint8_t*)msg.data(), msg.size());
}

void Lsw2RelayHost::drop(int n, bool tell) {
    {
        std::lock_guard<std::mutex> hold(mu_);
        auto it = peers_map_.find(n);
        if (it == peers_map_.end()) return;
        if (it->second->sock != LSW2_INVALID) lsw2_close(it->second->sock);
        peers_map_.erase(it);
        peers_ = int(peers_map_.size());
    }
    if (tell) {
        const std::string m{char(2), char(n)};
        send_all_lanes(m);
    }
    std::fprintf(stderr, "[relay] host: relayed player %d disconnected\n", n);
}

void Lsw2RelayHost::handle(int lane, const std::string& msg) {
    if (msg.size() < 2) return;
    const uint8_t kind = uint8_t(msg[0]);
    const int n = uint8_t(msg[1]);
    if (kind == 1) {
        std::lock_guard<std::mutex> hold(mu_);
        if (peers_map_.count(n)) return;               /* every lane announces it */
        /* A joiner arrived: give it a connection to our own listen port, which
         * the transport accepts like any other. */
        socket_t s = connect_within("127.0.0.1", listen_port_, 2000);
        if (s == LSW2_INVALID) {
            std::fprintf(stderr, "[relay] host: could not hand player %d to the lobby\n", n);
            const std::string m{char(2), char(n)};
            lanes_[size_t(lane)]->send_binary((const uint8_t*)m.data(), m.size());
            return;
        }
        auto p = std::make_unique<Peer>();
        p->sock = s;
        peers_map_[n] = std::move(p);
        peers_ = int(peers_map_.size());
        std::fprintf(stderr, "[relay] host: a player joined through the relay (peer %d, "
                             "%d lanes)\n", n, lanes_n_);
    } else if (kind == 0) {
        std::string deliver;
        bool broken = false;
        {
            std::lock_guard<std::mutex> hold(mu_);
            auto it = peers_map_.find(n);
            if (it == peers_map_.end()) return;
            if (!it->second->stream.take(msg.substr(2), &deliver)) broken = true;
            else if (!deliver.empty() &&
                     !send_all(it->second->sock, deliver.data(), deliver.size())) broken = true;
        }
        if (broken) drop(n, true);
    } else if (kind == 2) {
        drop(n, false);
    }
}

void Lsw2RelayHost::lane_run(int lane) {
    Lsw2WsClient& ws = *lanes_[size_t(lane)];
    int backoff_ms = 1000;
    std::string last_error;
    const std::string path = "/v1/relay/" + id_ + "?role=host&v=2&lane=" + std::to_string(lane);
    while (!stop_) {
        std::string error;
        if (!ws.open(host_, port_, path, token_, 5000, &error)) {
            if (error != last_error) {
                std::fprintf(stderr, "[relay] host: lane %d cannot open the relay (%s) - "
                                     "retrying\n", lane, error.c_str());
                last_error = error;
            }
            for (int waited = 0; waited < backoff_ms && !stop_; waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff_ms = std::min(backoff_ms * 2, 30000);
            continue;
        }
        backoff_ms = 1000;
        last_error.clear();
        if (++lanes_up_ == 1)
            std::fprintf(stderr, "[relay] host: online - players who cannot reach this "
                                 "machine directly join through the relay\n");
        std::vector<Lsw2WsMessage> early;
        ws.take_early(&early);
        for (const Lsw2WsMessage& m : early) if (m.opcode == 2) handle(lane, m.data);
        auto last_ping = Clock::now();
        while (!stop_ && ws.is_open()) {
            if (wait_readable(as_sock(ws.fd()), 100)) {
                std::vector<Lsw2WsMessage> msgs;
                const bool open = ws.receive(&msgs);
                for (const Lsw2WsMessage& m : msgs) if (m.opcode == 2) handle(lane, m.data);
                if (!open) break;
            }
            if (ms_since(last_ping) > kKeepaliveMs) {
                ws.send_text("ping");
                last_ping = Clock::now();
            }
        }
        ws.close();
        if (--lanes_up_ == 0 && !stop_) {
            /* Every lane is down: the room has closed its joiners' streams. */
            std::vector<int> all;
            {
                std::lock_guard<std::mutex> hold(mu_);
                for (auto& p : peers_map_) all.push_back(p.first);
            }
            for (int n : all) drop(n, false);
        }
        if (!stop_) {
            std::fprintf(stderr, "[relay] host: lane %d dropped - reconnecting\n", lane);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void Lsw2RelayHost::local_run() {
    /* The lobby's side: whatever the transport sends a relayed joiner goes out
     * on every lane, numbered. */
    while (!stop_) {
        fd_set rd;
        FD_ZERO(&rd);
        socket_t top = 0;
        int count = 0;
        {
            std::lock_guard<std::mutex> hold(mu_);
            for (auto& p : peers_map_) {
                FD_SET(p.second->sock, &rd);
                if (p.second->sock > top) top = p.second->sock;
                ++count;
            }
        }
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        timeval tv{0, 20000};
        if (::select(int(top) + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        std::vector<int> gone;
        std::vector<std::string> out;
        {
            std::lock_guard<std::mutex> hold(mu_);
            for (auto& p : peers_map_) {
                if (!FD_ISSET(p.second->sock, &rd)) continue;
                char tmp[kRecvChunk];
                const int k = ::recv(p.second->sock, tmp, int(sizeof tmp), 0);
                if (k <= 0) { gone.push_back(p.first); continue; }
                std::string m{char(0), char(p.first)};
                m += p.second->stream.wrap(tmp, size_t(k));
                out.push_back(std::move(m));
            }
        }
        for (const std::string& m : out) send_all_lanes(m);
        for (int n : gone) drop(n, true);
    }
}

/* ------------------------------------------------------------- the joiner */

std::string Lsw2RelayJoin::lane_path(int lane) const {
    return "/v1/relay/" + id_ + "?role=join&v=2&lane=" + std::to_string(lane) + "&link=" + link_;
}

bool Lsw2RelayJoin::start(const std::string& master_host, int master_port,
                          const std::string& id, int timeout_ms, std::string* error) {
    stop();
    stop_ = false;
    ended_ = false;
    pending_.clear();
    stream_ = Lsw2LaneStream();
    host_ = master_host;
    port_ = master_port;
    id_ = id;
    lanes_n_ = lsw2_relay_lanes();
    {
        /* One nonce per join: the room gives all of this joiner's lanes one
         * peer number by it. */
        uint32_t r = uint32_t(Clock::now().time_since_epoch().count()) | 1u;
        char buf[24];
        const uint32_t a = next_rand(r), b = next_rand(r);
        std::snprintf(buf, sizeof buf, "%08x%08x", unsigned(a), unsigned(b));
        link_ = buf;
    }
    const auto t0 = Clock::now();
    lanes_.clear();
    for (int k = 0; k < lanes_n_; ++k) lanes_.push_back(std::make_unique<Lsw2WsClient>());

    /* LANE 0 MUST OPEN AND HEAR "ready"; the others are best effort here and
     * keep trying on their own threads. */
    Lsw2WsClient& first = *lanes_[0];
    if (!first.open(host_, port_, lane_path(0), "", timeout_ms, error)) return false;
    bool ready = false;
    std::vector<Lsw2WsMessage> early;
    first.take_early(&early);
    for (;;) {
        for (const Lsw2WsMessage& m : early) {
            if (m.opcode == 1 && m.data == "ready") ready = true;
            else if (m.opcode == 2) stream_.take(m.data, &pending_);
            else if (m.opcode == 8) {
                if (error) *error = close_text(m.data);
                first.close();
                return false;
            }
        }
        early.clear();
        if (ready) break;
        const int left = timeout_ms - ms_since(t0);
        if (left <= 0 || !wait_readable(as_sock(first.fd()), left)) {
            if (error) *error = "the relay did not answer";
            first.close();
            return false;
        }
        if (!first.receive(&early) && early.empty()) {
            if (error) *error = first.close_reason().empty() ? "the relay closed the connection"
                                                              : first.close_reason();
            return false;
        }
    }
    for (int k = 1; k < lanes_n_; ++k) {
        std::string ignored;
        if (lanes_[size_t(k)]->open(host_, port_, lane_path(k), "", 2000, &ignored)) {
            std::vector<Lsw2WsMessage> e;
            lanes_[size_t(k)]->take_early(&e);   /* its "ready"; data only follows later */
        }
    }

    socket_t l = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t alen = sizeof a;
    if (l == LSW2_INVALID || ::bind(l, (sockaddr*)&a, sizeof a) != 0 || ::listen(l, 1) != 0 ||
        getsockname(l, (sockaddr*)&a, &alen) != 0) {
        if (l != LSW2_INVALID) lsw2_close(l);
        for (auto& w : lanes_) w->close();
        if (error) *error = "could not open a local port for the relay";
        return false;
    }
    listen_fd_ = as_fd(l);
    local_port_ = ntohs(a.sin_port);
    alive_ = true;
    for (int k = 0; k < lanes_n_; ++k) threads_.emplace_back([this, k] { lane_run(k); });
    threads_.emplace_back([this] { local_run(); });
    int up = 0;
    for (auto& w : lanes_) up += w->is_open() ? 1 : 0;
    std::fprintf(stderr, "[relay] join: connected to the lobby through the relay "
                         "(%d of %d lanes, local port %u)\n", up, lanes_n_, unsigned(local_port_));
    return true;
}

void Lsw2RelayJoin::stop() {
    stop_ = true;
    for (auto& w : lanes_) if (w) w->close();
    for (std::thread& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
    if (listen_fd_ >= 0) lsw2_close(as_sock(listen_fd_));
    listen_fd_ = -1;
    const long long lf = local_fd_.exchange(-1);
    if (lf >= 0) lsw2_close(as_sock(lf));
    alive_ = false;
}

void Lsw2RelayJoin::deliver(const std::string& bytes) {
    const long long lf = local_fd_.load();
    if (lf < 0) { pending_ += bytes; return; }
    if (!send_all(as_sock(lf), bytes.data(), bytes.size())) ended_ = true;
}

void Lsw2RelayJoin::send_all_lanes(const std::string& msg) {
    const bool test = test_link().on();
    for (int k = 0; k < lanes_n_; ++k) {
        Lsw2WsClient& w = *lanes_[size_t(k)];
        if (!w.is_open()) continue;
        if (test) {
            TestLane& t = g_test_lanes[k];
            std::lock_guard<std::mutex> hold(t.mu);
            t.up.push_back({t.due_now(t.last_up), msg});
        } else {
            w.send_binary((const uint8_t*)msg.data(), msg.size());
        }
    }
}

void Lsw2RelayJoin::lane_run(int lane) {
    Lsw2WsClient& ws = *lanes_[size_t(lane)];
    const bool test = test_link().on();
    TestLane& tl = g_test_lanes[lane];
    {
        /* Each lane its own luck: real losses on separate connections are
         * independent, and lanes that froze together would test nothing. */
        std::lock_guard<std::mutex> hold(tl.mu);
        tl.rng = 0x12345u + uint32_t(lane + 1) * 2654435761u;
    }
    auto take = [&](const std::string& chunk) {
        std::lock_guard<std::mutex> hold(mu_);
        std::string out;
        if (!stream_.take(chunk, &out)) { ended_ = true; return; }
        if (!out.empty()) deliver(out);
    };
    auto last_ping = Clock::now();
    auto down_since = Clock::now();
    int backoff_ms = 1000;
    while (!stop_ && !ended_) {
        if (!ws.is_open()) {
            /* A LANE THAT DROPPED RECONNECTS - same link, so the room puts it
             * back on this joiner. The stream ends only if every lane stays
             * down (the room is gone, or the host left). */
            bool any = false;
            for (auto& w : lanes_) any = any || w->is_open();
            if (any) down_since = Clock::now();
            else if (ms_since(down_since) > 15000) { ended_ = true; break; }
            std::string ignored;
            if (ws.open(host_, port_, lane_path(lane), "", 4000, &ignored)) {
                std::vector<Lsw2WsMessage> e;
                ws.take_early(&e);
                for (const Lsw2WsMessage& m : e) if (m.opcode == 2) take(m.data);
                backoff_ms = 1000;
            } else {
                for (int waited = 0; waited < backoff_ms && !stop_; waited += 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                backoff_ms = std::min(backoff_ms * 2, 8000);
                continue;
            }
        }
        if (test) {
            std::vector<std::string> send_now, got_now;
            {
                std::lock_guard<std::mutex> hold(tl.mu);
                const auto now = Clock::now();
                while (!tl.up.empty() && tl.up.front().due <= now) {
                    send_now.push_back(std::move(tl.up.front().bytes));
                    tl.up.pop_front();
                }
                while (!tl.down.empty() && tl.down.front().due <= now) {
                    got_now.push_back(std::move(tl.down.front().bytes));
                    tl.down.pop_front();
                }
            }
            for (const std::string& m : send_now) ws.send_binary((const uint8_t*)m.data(), m.size());
            for (const std::string& m : got_now) take(m);
        }
        if (ws.is_open() && wait_readable(as_sock(ws.fd()), test ? 1 : 100)) {
            std::vector<Lsw2WsMessage> msgs;
            const bool open = ws.receive(&msgs);
            for (const Lsw2WsMessage& m : msgs) {
                if (m.opcode != 2) continue;
                if (test) {
                    std::lock_guard<std::mutex> hold(tl.mu);
                    tl.down.push_back({tl.due_now(tl.last_down), m.data});
                } else {
                    take(m.data);
                }
            }
            if (!open) {
                std::fprintf(stderr, "[relay] join: lane %d closed (%s)\n", lane,
                             ws.close_reason().c_str());
                /* The room says why: a host that left ends the stream. */
                if (ws.close_reason().find("host") != std::string::npos) ended_ = true;
                continue;
            }
        }
        if (ws.is_open() && ms_since(last_ping) > kKeepaliveMs) {
            ws.send_text("ping");
            last_ping = Clock::now();
        }
    }
    ws.close();
}

void Lsw2RelayJoin::local_run() {
    /* Accept the transport's connection, hand it what arrived first, then send
     * everything it writes out on every lane, numbered. */
    socket_t local = LSW2_INVALID;
    while (!stop_ && !ended_ && local == LSW2_INVALID) {
        if (!wait_readable(as_sock(listen_fd_), 100)) continue;
        local = ::accept(as_sock(listen_fd_), nullptr, nullptr);
    }
    if (local != LSW2_INVALID) {
        set_nodelay(local);
        set_send_timeout(local, 5000);
        std::lock_guard<std::mutex> hold(mu_);
        if (!pending_.empty() && !send_all(local, pending_.data(), pending_.size())) ended_ = true;
        pending_.clear();
        local_fd_ = as_fd(local);
    }
    while (!stop_ && !ended_ && local != LSW2_INVALID) {
        if (!wait_readable(local, 100)) continue;
        char tmp[kRecvChunk];
        const int k = ::recv(local, tmp, int(sizeof tmp), 0);
        if (k <= 0) break;                                /* the transport left */
        std::string m;
        {
            std::lock_guard<std::mutex> hold(mu_);
            m = stream_.wrap(tmp, size_t(k));
        }
        send_all_lanes(m);
    }
    ended_ = true;
    alive_ = false;
    const long long lf = local_fd_.exchange(-1);
    if (lf >= 0) lsw2_close(as_sock(lf));
    std::fprintf(stderr, "[relay] join: the relayed stream ended\n");
}
