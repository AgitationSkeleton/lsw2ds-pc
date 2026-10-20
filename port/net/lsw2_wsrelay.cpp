/* See lsw2_wsrelay.h. */

#include "lsw2_wsrelay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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
std::string close_reason(const std::string& payload) {
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
    if (fd_ < 0) return false;
    const std::string f = lsw2_ws_frame(opcode, data, n, next_rand(rng_));
    if (!send_all(as_sock(fd_), f.data(), f.size())) {
        close();
        return false;
    }
    return true;
}

bool Lsw2WsClient::send_binary(const uint8_t* data, size_t n) { return send_frame(2, data, n); }

bool Lsw2WsClient::send_text(const std::string& text) {
    return send_frame(1, (const uint8_t*)text.data(), text.size());
}

bool Lsw2WsClient::receive(std::vector<Lsw2WsMessage>* out) {
    if (fd_ < 0) return false;
    char tmp[kRecvChunk];
    const int k = ::recv(as_sock(fd_), tmp, int(sizeof tmp), 0);
    if (k <= 0) { close(); return false; }
    buf_.append(tmp, size_t(k));
    std::vector<Lsw2WsMessage> got;
    /* Frames already buffered (from open) are parsed on the first call too. */
    if (!lsw2_ws_parse(buf_, partial_, partial_op_, &got)) { close(); return false; }
    bool open = true;
    for (Lsw2WsMessage& m : got) {
        if (m.opcode == 9) {                          /* ping -> pong */
            send_frame(10, (const uint8_t*)m.data.data(), m.data.size());
            continue;
        }
        if (m.opcode == 10) continue;
        if (m.opcode == 8) open = false;
        out->push_back(std::move(m));
    }
    if (!open) {
        send_frame(8, nullptr, 0);
        close();
    }
    return open;
}

void Lsw2WsClient::close() {
    if (fd_ >= 0) lsw2_close(as_sock(fd_));
    fd_ = -1;
}

/* ------------------------------------------------------------- the host */

Lsw2RelayHost::Lsw2RelayHost(std::string master_host, int master_port, std::string id,
                             std::string token, uint16_t listen_port)
    : host_(std::move(master_host)), id_(std::move(id)), token_(std::move(token)),
      port_(master_port), listen_port_(listen_port) {}

void Lsw2RelayHost::start() {
    if (thread_.joinable() || id_.empty() || token_.empty()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void Lsw2RelayHost::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    online_ = false;
    peers_ = 0;
}

void Lsw2RelayHost::run() {
    int backoff_ms = 1000;
    std::string last_error;
    while (!stop_) {
        Lsw2WsClient ws;
        std::string error;
        if (!ws.open(host_, port_, "/v1/relay/" + id_ + "?role=host", token_, 5000, &error)) {
            if (error != last_error) {
                std::fprintf(stderr, "[relay] host: cannot open the relay (%s) - retrying\n",
                             error.c_str());
                last_error = error;
            }
            for (int waited = 0; waited < backoff_ms && !stop_; waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff_ms = std::min(backoff_ms * 2, 30000);
            continue;
        }
        online_ = true;
        backoff_ms = 1000;
        last_error.clear();
        {
            std::vector<Lsw2WsMessage> early;   /* nothing is expected; drop it */
            ws.take_early(&early);
        }
        std::fprintf(stderr, "[relay] host: online - players who cannot reach this "
                             "machine directly join through the relay\n");

        std::map<int, socket_t> peers;
        auto drop = [&](int n, bool tell) {
            auto it = peers.find(n);
            if (it == peers.end()) return;
            lsw2_close(it->second);
            peers.erase(it);
            if (tell) {
                const uint8_t m[2] = {2, uint8_t(n)};
                ws.send_binary(m, 2);
            }
            std::fprintf(stderr, "[relay] host: relayed player %d disconnected\n", n);
        };
        auto last_sent = Clock::now();
        std::string out;

        while (!stop_ && ws.is_open()) {
            fd_set rd;
            FD_ZERO(&rd);
            socket_t top = as_sock(ws.fd());
            FD_SET(top, &rd);
            for (auto& p : peers) {
                FD_SET(p.second, &rd);
                if (p.second > top) top = p.second;
            }
            timeval tv{0, 200000};
            if (::select(int(top) + 1, &rd, nullptr, nullptr, &tv) < 0) break;

            if (FD_ISSET(as_sock(ws.fd()), &rd)) {
                std::vector<Lsw2WsMessage> msgs;
                const bool open = ws.receive(&msgs);
                for (const Lsw2WsMessage& m : msgs) {
                    if (m.opcode != 2 || m.data.size() < 2) continue;
                    const uint8_t kind = uint8_t(m.data[0]);
                    const int n = uint8_t(m.data[1]);
                    if (kind == 1) {
                        /* A joiner arrived: give it a connection to our own
                         * listen port, which the transport accepts like any. */
                        drop(n, false);
                        socket_t s = connect_within("127.0.0.1", listen_port_, 2000);
                        if (s == LSW2_INVALID) {
                            const uint8_t c[2] = {2, uint8_t(n)};
                            ws.send_binary(c, 2);
                            std::fprintf(stderr, "[relay] host: could not hand player "
                                                 "%d to the lobby\n", n);
                        } else {
                            peers[n] = s;
                            std::fprintf(stderr, "[relay] host: a player joined through "
                                                 "the relay (peer %d)\n", n);
                        }
                    } else if (kind == 0) {
                        auto it = peers.find(n);
                        if (it != peers.end() &&
                            !send_all(it->second, m.data.data() + 2, m.data.size() - 2))
                            drop(n, true);
                    } else if (kind == 2) {
                        drop(n, false);
                    }
                }
                if (!open) break;
            }

            std::vector<int> gone;
            for (auto& p : peers) {
                if (!FD_ISSET(p.second, &rd)) continue;
                char tmp[kRecvChunk];
                const int k = ::recv(p.second, tmp, int(sizeof tmp), 0);
                if (k <= 0) { gone.push_back(p.first); continue; }
                out.assign(2, '\0');
                out[1] = char(p.first);
                out.append(tmp, size_t(k));
                ws.send_binary((const uint8_t*)out.data(), out.size());
                last_sent = Clock::now();
            }
            for (int n : gone) drop(n, true);
            peers_ = int(peers.size());

            if (ms_since(last_sent) > kKeepaliveMs) {
                ws.send_text("ping");
                last_sent = Clock::now();
            }
        }

        for (auto& p : peers) lsw2_close(p.second);
        peers_ = 0;
        online_ = false;
        ws.close();
        if (!stop_) {
            std::fprintf(stderr, "[relay] host: the relay connection dropped - reconnecting\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

/* ------------------------------------------------------------- the joiner */

bool Lsw2RelayJoin::start(const std::string& master_host, int master_port,
                          const std::string& id, int timeout_ms, std::string* error) {
    stop();
    stop_ = false;
    pending_.clear();
    const auto t0 = Clock::now();

    if (!ws_.open(master_host, master_port, "/v1/relay/" + id + "?role=join", "",
                  timeout_ms, error))
        return false;

    /* WAIT FOR THE ROOM TO SAY THE HOST IS THERE. Without a host it closes the
     * socket with the reason, which is what the player is shown. */
    bool ready = false;
    {
        std::vector<Lsw2WsMessage> early;
        ws_.take_early(&early);
        for (const Lsw2WsMessage& m : early) {
            if (m.opcode == 1 && m.data == "ready") ready = true;
            else if (m.opcode == 2) pending_ += m.data;
            else if (m.opcode == 8) {
                if (error) *error = close_reason(m.data);
                ws_.close();
                return false;
            }
        }
    }
    while (!ready) {
        const int left = timeout_ms - ms_since(t0);
        if (left <= 0 || !wait_readable(as_sock(ws_.fd()), left)) {
            if (error) *error = "the relay did not answer";
            ws_.close();
            return false;
        }
        std::vector<Lsw2WsMessage> msgs;
        const bool open = ws_.receive(&msgs);
        for (const Lsw2WsMessage& m : msgs) {
            if (m.opcode == 1 && m.data == "ready") ready = true;
            else if (m.opcode == 2) pending_ += m.data;
            else if (m.opcode == 8 && error) *error = close_reason(m.data);
        }
        if (!open && !ready) {
            if (error && error->empty()) *error = "the relay closed the connection";
            return false;
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
        ws_.close();
        if (error) *error = "could not open a local port for the relay";
        return false;
    }
    listen_fd_ = as_fd(l);
    local_port_ = ntohs(a.sin_port);
    alive_ = true;
    thread_ = std::thread([this] { run(); });
    std::fprintf(stderr, "[relay] join: connected to the lobby through the relay "
                         "(local port %u)\n", unsigned(local_port_));
    return true;
}

void Lsw2RelayJoin::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) lsw2_close(as_sock(listen_fd_));
    listen_fd_ = -1;
    ws_.close();
    alive_ = false;
}

void Lsw2RelayJoin::run() {
    socket_t local = LSW2_INVALID;
    auto last_sent = Clock::now();
    while (!stop_ && ws_.is_open()) {
        fd_set rd;
        FD_ZERO(&rd);
        socket_t wsfd = as_sock(ws_.fd());
        socket_t top = wsfd;
        FD_SET(wsfd, &rd);
        socket_t watch = local != LSW2_INVALID ? local : as_sock(listen_fd_);
        if (watch != LSW2_INVALID) {
            FD_SET(watch, &rd);
            if (watch > top) top = watch;
        }
        timeval tv{0, 200000};
        if (::select(int(top) + 1, &rd, nullptr, nullptr, &tv) < 0) break;

        if (local == LSW2_INVALID && watch != LSW2_INVALID && FD_ISSET(watch, &rd)) {
            local = ::accept(watch, nullptr, nullptr);
            if (local != LSW2_INVALID) {
                set_nodelay(local);
                set_send_timeout(local, 5000);
                if (!pending_.empty() && !send_all(local, pending_.data(), pending_.size())) break;
                pending_.clear();
            }
        } else if (local != LSW2_INVALID && FD_ISSET(local, &rd)) {
            char tmp[kRecvChunk];
            const int k = ::recv(local, tmp, int(sizeof tmp), 0);
            if (k <= 0) break;                       /* the transport left */
            if (!ws_.send_binary((const uint8_t*)tmp, size_t(k))) break;
            last_sent = Clock::now();
        }

        if (FD_ISSET(wsfd, &rd)) {
            std::vector<Lsw2WsMessage> msgs;
            const bool open = ws_.receive(&msgs);
            bool failed = false;
            for (const Lsw2WsMessage& m : msgs) {
                if (m.opcode != 2) continue;
                if (local == LSW2_INVALID) pending_ += m.data;
                else if (!send_all(local, m.data.data(), m.data.size())) { failed = true; break; }
            }
            if (failed) break;                       /* the transport stopped reading */
            if (!open) {
                std::fprintf(stderr, "[relay] join: the relay closed the lobby's stream\n");
                break;
            }
        }

        if (ms_since(last_sent) > kKeepaliveMs && ws_.is_open()) {
            ws_.send_text("ping");
            last_sent = Clock::now();
        }
    }
    if (local != LSW2_INVALID) lsw2_close(local);
    ws_.close();
    alive_ = false;
}
