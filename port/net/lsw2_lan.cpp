/* See lsw2_lan.h. */

#include "lsw2_lan.h"
#include "lsw2_name.h"

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
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#endif

namespace {

/* The handle is stored as long long so the header does not have to include
 * winsock. -1 is "closed" on both platforms; on Windows INVALID_SOCKET is
 * (SOCKET)~0, which as a signed long long is also -1. */
socket_t as_socket(long long h) { return static_cast<socket_t>(h); }

void set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void put_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
uint16_t get_u16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}
void put_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

/* HOW OFTEN THE BEACON SPEAKS. One second at 60fps. Slow enough to be free,
 * quick enough that a browser opened after a host started sees it before the
 * player decides nothing is there. */
constexpr int kTicksPerAnnounce = 60;

}  // namespace

/* ------------------------------------------------------------------------- */

Lsw2LanBeacon::~Lsw2LanBeacon() { stop(); }

bool Lsw2LanBeacon::start(const std::string& name, uint16_t game_port) {
    stop();

    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == LSW2_INVALID) return false;

    int on = 1;
    if (::setsockopt(s, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&on), sizeof on) != 0) {
        /* A machine that refuses broadcast can still host over the internet, so
         * this is a failure of LAN visibility and not of hosting. */
        lsw2_close(s);
        return false;
    }
    set_nonblocking(s);

    /* BIND SO THE SOURCE PORT IS OURS AND STABLE. An unbound UDP socket picks a
     * port on its first send, which would be fine - but binding explicitly is
     * what makes that port readable here, and it is the beacon's instance id.
     * Port 0 asks the OS for a free one. */
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
        lsw2_close(s);
        return false;
    }
#ifdef _WIN32
    int me_len = sizeof me;
#else
    socklen_t me_len = sizeof me;
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &me_len) != 0) {
        lsw2_close(s);
        return false;
    }
    id_ = ntohs(me.sin_port);

    sock_ = static_cast<long long>(s);
    /* SANITISED HERE TOO. The host's own name has already been through the
     * sanitiser on its way into the session, but this is a wire format and the
     * rule for those is that the encoder does not assume its input. */
    char clean[kLsw2LanNameBytes];
    lsw2_sanitise_name(name.c_str(), clean, sizeof clean, "Player");
    name_ = clean;
    game_port_ = game_port;
    ticks_ = 0;
    return true;
}

void Lsw2LanBeacon::stop() {
    if (sock_ != -1) { lsw2_close(as_socket(sock_)); sock_ = -1; }
}

bool Lsw2LanBeacon::announce(int players, int max_players) {
    if (sock_ == -1) return false;

    uint8_t rec[kLsw2LanRecordBytes];
    std::memset(rec, 0, sizeof rec);
    put_u32(rec, kLsw2LanMagic);
    put_u16(rec + 4, game_port_);
    put_u16(rec + 6, uint16_t(players < 0 ? 0 : players));
    put_u16(rec + 8, uint16_t(max_players < 1 ? 1 : max_players));
    put_u16(rec + 10, id_);
    put_u16(rec + 12, uint16_t(name_.size()));
    std::memcpy(rec + 14, name_.data(),
                name_.size() < kLsw2LanNameBytes ? name_.size()
                                                 : kLsw2LanNameBytes);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kLsw2LanPort);

    /* TO THE BROADCAST ADDRESS *AND* TO LOOPBACK.
     *
     * Broadcast is what reaches the subnet. Loopback is what makes a host and a
     * browser on the SAME machine find each other - which is not only how this
     * is tested but how someone will first try it, running two copies of the
     * fork side by side. Many stacks do not loop a broadcast back to the
     * sending host, so relying on broadcast alone would make the most obvious
     * manual test fail while the feature worked. */
    bool any = false;
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (::sendto(as_socket(sock_), reinterpret_cast<const char*>(rec),
                 sizeof rec, 0, reinterpret_cast<sockaddr*>(&to),
                 sizeof to) > 0) any = true;

    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::sendto(as_socket(sock_), reinterpret_cast<const char*>(rec),
                 sizeof rec, 0, reinterpret_cast<sockaddr*>(&to),
                 sizeof to) > 0) any = true;

    return any;
}

void Lsw2LanBeacon::tick(int players, int max_players) {
    if (sock_ == -1) return;
    if (++ticks_ < kTicksPerAnnounce) return;
    ticks_ = 0;
    announce(players, max_players);
}

/* ------------------------------------------------------------------------- */

Lsw2LanBrowser::~Lsw2LanBrowser() { stop(); }

bool Lsw2LanBrowser::start() {
    stop();

    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == LSW2_INVALID) return false;

    /* REUSE, so a host and a browser can both hold this port on one machine.
     * Without it the second of the two fails to bind and LAN discovery appears
     * broken exactly in the configuration someone tests first. */
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof on);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kLsw2LanPort);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        lsw2_close(s);
        return false;
    }
    set_nonblocking(s);

    sock_ = static_cast<long long>(s);
    servers_.clear();
    last_seen_.clear();
    ids_.clear();
    return true;
}

void Lsw2LanBrowser::stop() {
    if (sock_ != -1) { lsw2_close(as_socket(sock_)); sock_ = -1; }
}

void Lsw2LanBrowser::poll() {
    if (sock_ == -1) return;

    for (int& n : last_seen_) ++n;

    for (;;) {
        uint8_t rec[kLsw2LanRecordBytes];
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof from;
#else
        socklen_t from_len = sizeof from;
#endif
        const int n = ::recvfrom(as_socket(sock_),
                                 reinterpret_cast<char*>(rec), sizeof rec, 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) break;

        /* EVERY FIELD CHECKED. Anything on the subnet can send this, so the
         * only rule that matters is that a hostile datagram cannot do worse
         * than be ignored. A short read is not a truncated record to be
         * salvaged - it is not our format. */
        if (n != int(sizeof rec)) continue;
        if (get_u32(rec) != kLsw2LanMagic) continue;

        const uint16_t game_port = get_u16(rec + 4);
        if (game_port == 0) continue;

        int players = int(get_u16(rec + 6));
        int max_players = int(get_u16(rec + 8));
        if (max_players < 1 || max_players > 4) max_players = 4;
        if (players < 0) players = 0;
        if (players > max_players) players = max_players;

        const uint16_t id = get_u16(rec + 10);

        /* OUR OWN ANNOUNCEMENT IS NOT A GAME WE CAN JOIN. A host broadcasts to
         * the subnet and the subnet includes itself, so without this a host
         * running a browser would list its own lobby - and two copies on one
         * machine, which is how anyone tests this, would each show two rows
         * where there is one other game. */
        if (ignore_ != 0 && id == ignore_) continue;

        /* The length field is advice, not authority: it is clamped to the
         * fixed field rather than trusted to index into it. */
        int len = int(get_u16(rec + 12));
        if (len < 0 || len > kLsw2LanNameBytes) len = kLsw2LanNameBytes;

        char raw[kLsw2LanNameBytes + 1];
        std::memcpy(raw, rec + 14, size_t(len));
        raw[len] = '\0';
        char clean[kLsw2LanNameBytes];
        lsw2_sanitise_name(raw, clean, sizeof clean, "Player");

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);

        /* MERGE BY INSTANCE ID, not by address.
         *
         * A host announcing once a second must update its row rather than add
         * one a second until the list is useless - and, more subtly, the two
         * copies of a single announcement arrive from two different source
         * addresses and must not become two hosts. The id is what makes them
         * the same host; see the header.
         *
         * The game port is folded into the key so that one machine hosting two
         * lobbies is still two rows. */
        const uint32_t key = (uint32_t(id) << 16) | uint32_t(game_port);
        const bool loopback = std::strcmp(ip, "127.0.0.1") == 0;

        bool found = false;
        for (size_t i = 0; i < servers_.size(); ++i) {
            if (ids_[i] != key) continue;
            servers_[i].name = clean;
            servers_[i].players = players;
            servers_[i].max_players = max_players;
            /* PREFER THE ROUTABLE ADDRESS. Both copies describe the same host,
             * but only one of them names an address another machine could use -
             * and on this machine either works, so taking the non-loopback one
             * is right in both cases. */
            if (!loopback) servers_[i].address = ip;
            last_seen_[i] = 0;
            found = true;
            break;
        }
        if (found) continue;

        Lsw2ServerEntry e;
        e.name = clean;
        e.address = ip;
        e.port = int(game_port);
        e.players = players;
        e.max_players = max_players;
        servers_.push_back(e);
        last_seen_.push_back(0);
        ids_.push_back(key);
    }
}

void Lsw2LanBrowser::expire(int missed_polls) {
    const int limit = missed_polls;
    for (size_t i = servers_.size(); i-- > 0;) {
        if (last_seen_[i] > limit) {
            servers_.erase(servers_.begin() + long(i));
            last_seen_.erase(last_seen_.begin() + long(i));
            ids_.erase(ids_.begin() + long(i));
        }
    }
}
