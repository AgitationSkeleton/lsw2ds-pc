/* See lsw2_tcp.h. */

#include "lsw2_tcp.h"

#include <algorithm>

#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define LSW2_INVALID INVALID_SOCKET
#  define lsw2_close closesocket
#  define lsw2_would_block() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#  define lsw2_would_block() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

namespace {

/* How long a join waits for the host to answer (see join()). */
constexpr int kConnectTimeoutMs = 3000;

void set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void set_nodelay(socket_t s) {
    /* NAGLE OFF, AND THIS IS NOT AN OPTIMISATION - it is correctness for this
     * traffic. Nagle holds a small write back until the previous one is
     * acknowledged, which on a 132-byte-per-frame stream means the block for
     * frame N can sit in the kernel waiting for frame N-1's ack. That converts
     * a lockstep session's steady cadence into a sawtooth, and it looks exactly
     * like network jitter. */
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

void put_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
uint16_t get_u16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

void put_u32(uint8_t* p, uint32_t v) {
    /* LITTLE-ENDIAN BY HAND. Both ends of this wire are the port, and the port
     * runs on little-endian hosts - but writing the bytes explicitly costs
     * nothing and means a big-endian build would interoperate rather than
     * silently disagree. */
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

struct Lsw2TcpTransport::Peer {
    socket_t fd = LSW2_INVALID;
    int station = -1;
    /* WHERE THIS PEER CONNECTED FROM, taken from accept() rather than from
     * anything the peer says. It is reported to the directory as part of the
     * roster so an operator's "Ban IP" has something to act on - the directory
     * itself never sees a joiner, because joiners connect here. Read-only to
     * the rest of the port: nothing authorises off it. */
    std::string address;
    /* A partial record. Bounded by the record size, so a peer cannot make this
     * grow - which is the other thing fixed-size framing buys. */
    uint8_t buf[kLsw2RecordBytes] = {0};
    int have = 0;
    bool dead = false;
    /* Bytes accepted for this peer that the socket has not taken yet - always
     * whole records at the back, possibly a partly-sent one at the front. */
    std::vector<uint8_t> out;
};

void Lsw2TcpTransport::flush(Peer& p) {
    if (p.dead || p.fd == LSW2_INVALID || p.out.empty()) return;
    size_t sent = 0;
    while (sent < p.out.size()) {
        const int n = int(::send(socket_t(p.fd),
                                 (const char*)p.out.data() + sent,
                                 int(p.out.size() - sent), 0));
        if (n > 0) { sent += size_t(n); continue; }
        if (n < 0 && !lsw2_would_block()) p.dead = true;
        break;
    }
    p.out.erase(p.out.begin(), p.out.begin() + long(sent));
    /* A peer that has not drained a megabyte is not coming back in time to
     * matter; treat it as gone rather than let the queue grow without bound. */
    if (p.out.size() > (1u << 20)) p.dead = true;
}

bool Lsw2TcpTransport::wait_readable(int timeout_us) {
    fd_set rd;
    FD_ZERO(&rd);
    socket_t top = 0;
    int count = 0;
    auto add = [&](socket_t fd) {
        FD_SET(fd, &rd);
        if (fd > top) top = fd;
        ++count;
    };
    if (is_host_ && listen_fd_ != -1) add(socket_t(listen_fd_));
    for (Peer* p : peers_)
        if (!p->dead && p->fd != LSW2_INVALID) add(p->fd);
    if (count == 0) return false;
    timeval tv;
    tv.tv_sec = timeout_us / 1000000;
    tv.tv_usec = timeout_us % 1000000;
    /* nfds is ignored on Windows and must be the highest fd + 1 elsewhere. */
    return ::select(int(top) + 1, &rd, nullptr, nullptr, &tv) > 0;
}

void Lsw2TcpTransport::enqueue(Peer& p, const uint8_t* record) {
    if (p.dead || p.fd == LSW2_INVALID) return;
    p.out.insert(p.out.end(), record, record + kLsw2RecordBytes);
    flush(p);
}

Lsw2TcpTransport::Lsw2TcpTransport() {
#ifdef _WIN32
    /* The runner already calls WSAStartup before boot, but this class is meant
     * to be usable from a test that does not. Refcounted, so a second call is
     * harmless. */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

Lsw2TcpTransport::~Lsw2TcpTransport() { close(); }

void Lsw2TcpTransport::close() {
    for (Peer* p : peers_) {
        if (p->fd != LSW2_INVALID) lsw2_close(socket_t(p->fd));
        delete p;
    }
    peers_.clear();
    if (listen_fd_ != -1) { lsw2_close(socket_t(listen_fd_)); listen_fd_ = -1; }
    connected_ = false;
}

uint16_t Lsw2TcpTransport::host(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == LSW2_INVALID) return 0;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) { lsw2_close(s); return 0; }
    if (listen(s, kLsw2MaxStations) != 0) { lsw2_close(s); return 0; }

    socklen_t len = sizeof a;
    getsockname(s, (sockaddr*)&a, &len);
    set_nonblocking(s);

    listen_fd_ = intptr_t(s);
    is_host_ = true;
    connected_ = true;
    assigned_ = 0;          /* the host is always station 0 */
    return ntohs(a.sin_port);
}

bool Lsw2TcpTransport::join(const std::string& address, uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == LSW2_INVALID) return false;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &a.sin_addr) != 1) {
        lsw2_close(s);
        return false;
    }
    /* A BOUNDED CONNECT, not a blocking one. This runs on the game's own
     * thread, and a blocking connect() to an address that never answers - a
     * host behind a VPN, or one whose port is not forwarded - waits out the
     * system's SYN retries: about 21 seconds on Windows, with the game frozen
     * the whole time from the moment the lobby is picked. A
     * reachable host answers in well under a second even across the internet,
     * so the attempt now gives up after kConnectTimeoutMs and the join fails
     * with its message instead. */
    set_nonblocking(s);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) {
#ifdef _WIN32
        const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool pending = errno == EINPROGRESS;
#endif
        if (!pending) { lsw2_close(s); return false; }
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);   /* Windows reports a refused connect here */
        timeval tv;
        tv.tv_sec = kConnectTimeoutMs / 1000;
        tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
        if (::select(int(s) + 1, nullptr, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex)) {
            lsw2_close(s);
            return false;
        }
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err != 0) {
            lsw2_close(s);
            return false;
        }
    }

    set_nodelay(s);
    Peer* p = new Peer();
    p->fd = s;
    p->station = 0;         /* the host */
    peers_.push_back(p);
    is_host_ = false;
    connected_ = true;
    assigned_ = -1;         /* until the host tells us */
    return true;
}

void Lsw2TcpTransport::drain(Peer& p) {
    for (;;) {
        const int want = kLsw2RecordBytes - p.have;
        const int n = int(recv(socket_t(p.fd), (char*)p.buf + p.have, want, 0));
        if (n == 0) { p.dead = true; return; }          /* orderly close */
        if (n < 0) { if (!lsw2_would_block()) p.dead = true; return; }
        p.have += n;
        if (p.have < kLsw2RecordBytes) return;          /* partial; wait */
        p.have = 0;

        uint8_t station = p.buf[4];
        const uint8_t flags = p.buf[5];

        /* flags bit 0 is the host's seat assignment, sent once on accept. It
         * carries no block and must not reach the session as one. */
        if (flags & 1u) {
            if (!is_host_) assigned_ = int(station);
            continue;
        }

        /* flags bit 1 is a host-settings record, and the same rule applies -
         * no block, so it must not reach the session as one.
         *
         * ACCEPTED ONLY FROM THE HOST. A client is connected to the host alone,
         * so anything it receives came from there; the host is connected to
         * several clients, and one of them sending this would be a client
         * dictating the match rules to the host. Refusing it here is the whole
         * defence, and it costs one comparison. */
        if (flags & 2u) {
            if (!is_host_) {
                std::memcpy(settings_, p.buf + 8, kLsw2ShareBytes);
                settings_pending_ = true;
            }
            continue;
        }
        /* flags bit 2 is a name. No block, like the other two.
         *
         * THE STATION IS THE SENDER'S, NOT THE RECORD'S, on the host side: a
         * client may name itself and nobody else. The host stamps the peer's
         * own station over whatever arrived before relaying, so a client that
         * tried to rename a rival would only ever rename itself. On the client
         * side the record comes from the host and its station is taken as
         * given, because the host is the one seating everybody. */
        if (flags & 4u) {
            NameNews news{};
            news.station = is_host_ ? p.station : int(station);
            std::memcpy(news.name, p.buf + 8, sizeof news.name - 1);
            news.name[sizeof news.name - 1] = '\0';
            if (news.station >= 0 && news.station < kLsw2MaxStations) {
                names_.push_back(news);
                if (is_host_) {
                    /* Relay with the sender's true station stamped in. */
                    uint8_t fixed[kLsw2RecordBytes];
                    std::memcpy(fixed, p.buf, kLsw2RecordBytes);
                    fixed[4] = uint8_t(news.station);
                    relay(fixed, news.station);
                }
            }
            continue;
        }

        /* flags bit 4 is a peer's customization parts. Both directions, host
         * relays with the sender's true station - the same rule the name record
         * follows, and for the sharper reason: a record that could name someone
         * else's station would let one player dress another. */
        if (flags & 16u) {
            PartsNews news{};
            news.station = is_host_ ? p.station : int(station);
            for (int i = 0; i < 5; ++i)
                news.parts[i] = get_u16(p.buf + 8 + i * 2);
            if (news.station >= 0 && news.station < kLsw2MaxStations) {
                parts_.push_back(news);
                if (is_host_) {
                    uint8_t fixed[kLsw2RecordBytes];
                    std::memcpy(fixed, p.buf, kLsw2RecordBytes);
                    fixed[4] = uint8_t(news.station);
                    relay(fixed, news.station);
                }
            }
            continue;
        }

        /* flags bit 3 is the shared RNG seed. Host only, for the same reason
         * the settings record is: a client dictating the seed would be a client
         * choosing what everyone's match rolls. */
        if (flags & 8u) {
            if (!is_host_) {
                seed_ = get_u32(p.buf + 8);
                seed_pending_ = true;
            }
            continue;
        }

        /* flags bit 5 is the host's match-start signal. Host only, for the same
         * reason the seed and settings are: only the host decides when the match
         * begins. No block. */
        if (flags & 32u) {
            if (!is_host_) {
                start_match_map_ = p.buf[8];
                start_match_rule_ = p.buf[9];
                start_match_seconds_ = get_u16(p.buf + 10);
                start_match_pending_ = true;
            }
            continue;
        }

        /* flags bit 6 is a relayed netgame message record. BOTH
         * directions, like the block: a client sends its own station's messages,
         * the host relays them on with the sender's station preserved. No block. */
        if (flags & 64u) {
            NetmsgNews news{};
            news.station = int(station);
            int n = int(p.buf[6]);
            if (n < 0) n = 0;
            if (n > kLsw2ShareBytes) n = kLsw2ShareBytes;
            news.len = n;
            std::memcpy(news.bytes, p.buf + 8, size_t(n));
            if (news.station >= 0 && news.station < kLsw2MaxStations) {
                netmsgs_.push_back(news);
                if (is_host_) relay(p.buf, news.station);
            }
            continue;
        }

        /* flags bit 7: the host-level WH's control records (lsw2_vwh.h). Host ->
         * client only; a host ignores one from a client. */
        if (flags & 128u) {
            if (!is_host_) {
                VwhNews n{p.buf[6], get_u32(p.buf), station, p.buf[7], {}};
                std::memcpy(n.payload, p.buf + 8, kLsw2ShareBytes);
                vwh_.push_back(n);
            } else if (p.buf[6] == kLsw2VwhPing && p.station > 0 &&
                       p.station < kLsw2MaxStations) {
                /* THE ONE CONTROL RECORD A HOST TAKES FROM A CLIENT - the echo
                 * of its own ping - stamped with the connection's station. */
                vwh_pongs_.push_back(VwhPong{p.station, get_u32(p.buf)});
            }
            continue;
        }
        if (station >= kLsw2MaxStations) continue;      /* refuse nonsense */
        /* ON THE HOST, A BLOCK IS ITS CONNECTION'S, whatever station byte it
         * claims - the rule the name and parts records already follow. The
         * comment below says the index is "the sender's station, not anything
         * the record chose"; before this line that was only true on a client. */
        if (is_host_) {
            if (p.station < 0 || p.station >= kLsw2MaxStations) continue;
            station = uint8_t(p.station);
            p.buf[4] = station;
        }

        /* WHO THE SENDER SAYS IT IS. Recorded before the block is queued so a
         * caller that reacts to the block already has it. A station only ever
         * states its OWN character - the index is the sender's station, not
         * anything the record chose - so this cannot be used to relabel
         * somebody else. */
        peer_character_[station] = p.buf[6];

        inbox_.emplace_back(p.buf, p.buf + kLsw2RecordBytes);
        /* THE HOST RELAYS. A client's block has to reach the other clients, and
         * only the host is connected to all of them. */
        if (is_host_) relay(p.buf, int(station));
    }
}

void Lsw2TcpTransport::relay(const uint8_t* record, int from_station) {
    for (Peer* q : peers_) {
        if (q->dead || q->station == from_station) continue;
        enqueue(*q, record);
    }
}

void Lsw2TcpTransport::send_seed(uint32_t seed) {
    if (!is_host_) return;
    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, 0);
    rec[4] = 0;
    rec[5] = 8;                          /* flags bit 3: the seed */
    put_u32(rec + 8, seed);
    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::take_seed(uint32_t* out) {
    if (!seed_pending_ || !out) return false;
    *out = seed_;
    seed_pending_ = false;
    return true;
}

void Lsw2TcpTransport::send_start_match(uint8_t map, uint8_t rule,
                                        uint16_t seconds) {
    if (!is_host_) return;
    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, 0);
    rec[4] = 0;                          /* the host's own station */
    rec[5] = 32;                         /* flags bit 5: match start */
    rec[8] = map;
    rec[9] = rule;
    put_u16(rec + 10, seconds);
    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::take_start_match(uint8_t* map, uint8_t* rule,
                                        uint16_t* seconds) {
    if (!start_match_pending_) return false;
    if (map) *map = start_match_map_;
    if (rule) *rule = start_match_rule_;
    if (seconds) *seconds = start_match_seconds_;
    start_match_pending_ = false;
    return true;
}

void Lsw2TcpTransport::send_netmsg(int station, const uint8_t* record,
                                   int len) {
    if (!record || len <= 0 || len > kLsw2ShareBytes) return;
    if (station < 0 || station >= kLsw2MaxStations) return;
    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, 0);
    rec[4] = uint8_t(station);           /* the ORIGINAL sender's station */
    rec[5] = 64;                         /* flags bit 6: a relayed netgame msg */
    rec[6] = uint8_t(len);
    std::memcpy(rec + 8, record, size_t(len));
    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::take_netmsg(int* station, uint8_t* out, int* len) {
    if (netmsgs_.empty() || !out || !len) return false;
    const NetmsgNews news = netmsgs_.front();
    netmsgs_.pop_front();
    if (station) *station = news.station;
    *len = news.len;
    std::memcpy(out, news.bytes, size_t(news.len));
    return true;
}

void Lsw2TcpTransport::send_name(const char* name) {
    if (!name) return;
    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, 0);
    rec[4] = uint8_t(is_host_ ? 0 : (assigned_ < 0 ? 0 : assigned_));
    rec[5] = 4;                          /* flags bit 2: a name */
    std::strncpy(reinterpret_cast<char*>(rec + 8), name, 23);

    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, rec);
    }
}

void Lsw2TcpTransport::send_name_as(int station, const char* name) {
    if (!is_host_ || !name) return;
    if (station < 0 || station >= kLsw2MaxStations) return;

    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, 0);
    rec[4] = uint8_t(station);
    rec[5] = 4;                          /* flags bit 2: a name */
    std::strncpy(reinterpret_cast<char*>(rec + 8), name, 23);

    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::take_name(int* station, char* out, int n) {
    if (names_.empty() || !out || n <= 0) return false;
    const NameNews news = names_.front();
    names_.pop_front();
    if (station) *station = news.station;
    std::strncpy(out, news.name, size_t(n - 1));
    out[n - 1] = '\0';
    return true;
}

void Lsw2TcpTransport::send_parts(const uint16_t* parts, int count) {
    if (!parts || count != 5) return;

    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, 0);
    rec[4] = uint8_t(is_host_ ? 0 : (assigned_ < 0 ? 0 : assigned_));
    rec[5] = 16;                         /* flags bit 4: customization parts */
    for (int i = 0; i < count; ++i) put_u16(rec + 8 + i * 2, parts[i]);

    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::take_parts(int* station, uint16_t* out, int count) {
    if (parts_.empty() || !out || count != 5) return false;
    const PartsNews news = parts_.front();
    parts_.pop_front();
    if (station) *station = news.station;
    for (int i = 0; i < count; ++i) out[i] = news.parts[i];
    return true;
}

void Lsw2TcpTransport::set_local_character(int character_id) {
    local_character_ = (character_id >= 0 && character_id < 0xFF)
                           ? uint8_t(character_id) : uint8_t(0xFF);
    /* The local station's own entry, so peer_character() answers consistently
     * for every station including this one. */
    const int me = is_host_ ? 0 : assigned_;
    if (me >= 0 && me < kLsw2MaxStations)
        peer_character_[me] = local_character_;
}

int Lsw2TcpTransport::peer_character(int station) const {
    if (station < 0 || station >= kLsw2MaxStations) return -1;
    const uint8_t v = peer_character_[station];
    return v == 0xFF ? -1 : int(v);
}

void Lsw2TcpTransport::broadcast_settings(const uint8_t* payload, int n) {
    /* HOST ONLY. A client calling this would be telling the host what the match
     * rules are, which is an authority the host has and nobody else. */
    if (!is_host_ || !payload || n <= 0) return;

    uint8_t record[kLsw2RecordBytes] = {0};
    put_u32(record, 0);
    record[4] = 0;                      /* the host's own station */
    record[5] = 2;                      /* flags bit 1: host settings */
    const int room = n < kLsw2ShareBytes ? n : kLsw2ShareBytes;
    std::memcpy(record + 8, payload, size_t(room));

    for (Peer* p : peers_) {
        if (p->dead || p->fd == LSW2_INVALID) continue;
        enqueue(*p, record);
    }
}

bool Lsw2TcpTransport::take_settings(uint8_t* out, int n) {
    if (!settings_pending_ || !out || n <= 0) return false;
    const int room = n < kLsw2ShareBytes ? n : kLsw2ShareBytes;
    std::memcpy(out, settings_, size_t(room));
    settings_pending_ = false;
    return true;
}

std::string Lsw2TcpTransport::peer_address(int station) const {
    for (const Peer* p : peers_)
        if (p->station == station && !p->dead) return p->address;
    return {};
}

void Lsw2TcpTransport::service() {
    if (is_host_ && listen_fd_ != -1) {
        for (;;) {
            sockaddr_in from{};
#ifdef _WIN32
            int from_len = int(sizeof from);
#else
            socklen_t from_len = sizeof from;
#endif
            socket_t c = accept(socket_t(listen_fd_),
                                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (c == LSW2_INVALID) break;
            if (int(peers_.size()) >= kLsw2MaxStations - 1) {
                /* A FIFTH PLAYER IS REFUSED AT THE DOOR rather than seated and
                 * then dropped. Four is the arena's width. */
                lsw2_close(c);
                continue;
            }
            set_nodelay(c);
            set_nonblocking(c);
            Peer* p = new Peer();
            p->fd = c;
            {
                char text[INET_ADDRSTRLEN] = {0};
                if (::inet_ntop(AF_INET, &from.sin_addr, text, sizeof text))
                    p->address = text;
            }
            /* Lowest free seat, so a vacated slot is reused - which is what the
             * game's own lobby rules expect. */
            bool taken[kLsw2MaxStations] = {true, false, false, false};
            for (Peer* q : peers_)
                if (q->station >= 0 && q->station < kLsw2MaxStations)
                    taken[q->station] = true;
            for (int i = 1; i < kLsw2MaxStations; ++i)
                if (!taken[i]) { p->station = i; break; }
            peers_.push_back(p);

            uint8_t hello[kLsw2RecordBytes] = {0};
            put_u32(hello, 0);
            hello[4] = uint8_t(p->station);
            hello[5] = 1;                       /* seat assignment */
            enqueue(*p, hello);
            joined_.push_back(p->station);
        }
    }

    for (Peer* p : peers_) if (!p->dead) flush(*p);
    for (Peer* p : peers_) if (!p->dead) drain(*p);

    for (size_t i = 0; i < peers_.size();) {
        if (!peers_[i]->dead) { ++i; continue; }
        if (peers_[i]->station >= 0) {
            lost_.push_back(peers_[i]->station);
            /* FORGET WHAT THAT SEAT WAS PLAYING AS.
             *
             * Seats are reused - the accept path picks the lowest free one - so
             * without this a new peer taking station 1 would read back as the
             * departed player's character until its first block arrived. One
             * frame of a stale answer, on the value remote avatars use to pick
             * a model: the wrong character would appear and then correct
             * itself, which reads as a graphical glitch rather than as state
             * nobody cleared. */
            const int seat = peers_[i]->station;
            if (seat < kLsw2MaxStations) peer_character_[seat] = 0xFF;
        }
        if (peers_[i]->fd != LSW2_INVALID) lsw2_close(socket_t(peers_[i]->fd));
        delete peers_[i];
        peers_.erase(peers_.begin() + long(i));
    }
}

void Lsw2TcpTransport::send(int frame, int from_station, const uint8_t* block) {
    uint8_t rec[kLsw2RecordBytes];
    put_u32(rec, uint32_t(frame));
    rec[4] = uint8_t(from_station);
    rec[5] = 0;
    rec[6] = local_character_;      /* who this station is playing as */
    rec[7] = 0;
    std::memcpy(rec + 8, block, kLsw2ShareBytes);

    /* THE LOCAL BLOCK GOES INTO OUR OWN INBOX TOO. On the DS a station's own
     * block is in the shared buffer alongside everyone else's, so the session
     * expects to receive it back - and not doing so would stall every frame
     * waiting for ourselves. */
    inbox_.emplace_back(rec, rec + kLsw2RecordBytes);

    for (Peer* p : peers_) {
        if (p->dead) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::poll(int* frame, int* from_station, uint8_t* block) {
    if (inbox_.empty()) return false;
    const std::vector<uint8_t> rec = inbox_.front();
    inbox_.pop_front();
    if (frame) *frame = int(get_u32(rec.data()));
    if (from_station) *from_station = int(rec[4]);
    if (block) std::memcpy(block, rec.data() + 8, kLsw2ShareBytes);
    return true;
}

void Lsw2TcpTransport::disconnect(int station) {
    for (Peer* p : peers_)
        if (p->station == station) p->dead = true;
}

void Lsw2TcpTransport::send_vwh(int to_station, uint8_t subtype, uint32_t frame,
                                uint8_t a, uint8_t b, const uint8_t* payload,
                                int n) {
    if (!is_host_) return;
    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, frame);
    rec[4] = a;
    rec[5] = 128;                        /* flags bit 7: host-level WH control */
    rec[6] = subtype;
    rec[7] = b;
    if (payload && n > 0)
        std::memcpy(rec + 8, payload, size_t(std::min(n, kLsw2ShareBytes)));
    for (Peer* p : peers_) {
        if (p->dead) continue;
        if (to_station >= 0 && p->station != to_station) continue;
        enqueue(*p, rec);
    }
}

bool Lsw2TcpTransport::take_vwh(uint8_t* subtype, uint32_t* frame, uint8_t* a,
                                uint8_t* b, uint8_t* payload) {
    if (vwh_.empty()) return false;
    const VwhNews n = vwh_.front();
    vwh_.pop_front();
    if (subtype) *subtype = n.subtype;
    if (frame) *frame = n.frame;
    if (a) *a = n.a;
    if (b) *b = n.b;
    if (payload) std::memcpy(payload, n.payload, kLsw2ShareBytes);
    return true;
}

void Lsw2TcpTransport::send_vwh_pong(uint32_t token) {
    if (is_host_) return;
    uint8_t rec[kLsw2RecordBytes] = {0};
    put_u32(rec, token);
    rec[5] = 128;                        /* flags bit 7: host-level WH control */
    rec[6] = kLsw2VwhPing;
    for (Peer* p : peers_)
        if (!p->dead) enqueue(*p, rec);
}

bool Lsw2TcpTransport::take_vwh_pong(int* station, uint32_t* token) {
    if (vwh_pongs_.empty()) return false;
    const VwhPong g = vwh_pongs_.front();
    vwh_pongs_.pop_front();
    if (station) *station = g.station;
    if (token) *token = g.token;
    return true;
}

int Lsw2TcpTransport::take_joined() {
    if (joined_.empty()) return -1;
    const int s = joined_.front(); joined_.pop_front(); return s;
}

int Lsw2TcpTransport::take_lost() {
    if (lost_.empty()) return -1;
    const int s = lost_.front(); lost_.pop_front(); return s;
}
