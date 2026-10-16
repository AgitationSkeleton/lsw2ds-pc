/* See lsw2_wireless_relay.h. */

#include "lsw2_wireless_relay.h"

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
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LSW2_INVALID (-1)
#  define lsw2_close ::close
#  define lsw2_would_block() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

namespace {

constexpr uint32_t kMaxFrameBytes = 0x1000;   /* the radio's frames are < 0x948 */

void set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

socket_t bind_udp(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == LSW2_INVALID) return LSW2_INVALID;
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof reuse);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        lsw2_close(s);
        return LSW2_INVALID;
    }
    set_nonblocking(s);
    return s;
}

}  // namespace

void Lsw2WirelessRelay::frame(const uint8_t* payload, int len,
                              std::vector<uint8_t>& out) {
    const uint32_t n = uint32_t(len);
    out.push_back(uint8_t(n & 0xFF));
    out.push_back(uint8_t((n >> 8) & 0xFF));
    out.push_back(uint8_t((n >> 16) & 0xFF));
    out.push_back(uint8_t((n >> 24) & 0xFF));
    if (len > 0) out.insert(out.end(), payload, payload + len);
}

int Lsw2WirelessRelay::deframe(std::vector<uint8_t>& buf,
                               std::vector<std::vector<uint8_t>>& out) {
    int delivered = 0;
    size_t off = 0;
    while (buf.size() - off >= 4) {
        const uint32_t n = uint32_t(buf[off]) | (uint32_t(buf[off + 1]) << 8) |
                           (uint32_t(buf[off + 2]) << 16) |
                           (uint32_t(buf[off + 3]) << 24);
        if (n > kMaxFrameBytes) {
            /* A length past the cap is a desync on the link; drop everything
             * buffered rather than trust a wild size. */
            buf.clear();
            return delivered;
        }
        if (buf.size() - off < 4 + n) break;   /* partial - wait for more */
        out.emplace_back(buf.begin() + off + 4, buf.begin() + off + 4 + n);
        off += 4 + n;
        ++delivered;
    }
    if (off > 0) buf.erase(buf.begin(), buf.begin() + off);
    return delivered;
}

bool Lsw2WirelessRelay::start(uint16_t catch_port, uint16_t inject_port,
                              uintptr_t link_socket) {
    stop();
    const socket_t c = bind_udp(catch_port);
    if (c == LSW2_INVALID) return false;
    const socket_t inj = socket(AF_INET, SOCK_DGRAM, 0);
    if (inj == LSW2_INVALID) {
        lsw2_close(c);
        return false;
    }
    set_nonblocking(inj);
    catch_sock_ = uintptr_t(c);
    inject_sock_ = uintptr_t(inj);
    inject_port_ = inject_port;
    link_ = link_socket;
    link_buf_.clear();
    running_ = true;
    return true;
}

bool Lsw2WirelessRelay::service() {
    if (!running_) return false;
    const socket_t c = socket_t(catch_sock_);
    const socket_t inj = socket_t(inject_sock_);
    const socket_t link = socket_t(link_);
    bool link_ok = true;

    /* LOCAL INSTANCE -> PEER: drain every datagram the local instance sent to
     * the peer's slot and length-prefix it onto the TCP link. */
    for (;;) {
        uint8_t datagram[kMaxFrameBytes];
        const int got = recv(c, reinterpret_cast<char*>(datagram),
                             sizeof datagram, 0);
        if (got < 0) {
            if (lsw2_would_block()) break;
            break;
        }
        if (got == 0) break;
        std::vector<uint8_t> out;
        frame(datagram, got, out);
        size_t sent = 0;
        while (sent < out.size()) {
            const int w = send(link, reinterpret_cast<const char*>(out.data() + sent),
                               int(out.size() - sent), 0);
            if (w <= 0) {
                if (w < 0 && lsw2_would_block()) continue;   /* backpressure */
                link_ok = false;
                break;
            }
            sent += size_t(w);
        }
        if (!link_ok) break;
        ++tx_;
    }

    /* PEER -> LOCAL INSTANCE: pull bytes off the TCP link, deframe whole
     * datagrams, and deliver each to the local instance's listening port. */
    for (;;) {
        char chunk[4096];
        const int got = recv(link, chunk, sizeof chunk, 0);
        if (got == 0) {
            link_ok = false;
            break;
        }
        if (got < 0) {
            if (lsw2_would_block()) break;
            link_ok = false;
            break;
        }
        link_buf_.insert(link_buf_.end(), chunk, chunk + got);
    }
    std::vector<std::vector<uint8_t>> frames;
    deframe(link_buf_, frames);
    for (const std::vector<uint8_t>& f : frames) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dst.sin_port = htons(inject_port_);
        sendto(inj, reinterpret_cast<const char*>(f.data()), int(f.size()), 0,
               reinterpret_cast<sockaddr*>(&dst), sizeof dst);
        ++rx_;
    }

    return link_ok;
}

void Lsw2WirelessRelay::stop() {
    if (catch_sock_ != ~uintptr_t(0)) {
        lsw2_close(socket_t(catch_sock_));
        catch_sock_ = ~uintptr_t(0);
    }
    if (inject_sock_ != ~uintptr_t(0)) {
        lsw2_close(socket_t(inject_sock_));
        inject_sock_ = ~uintptr_t(0);
    }
    running_ = false;
    link_buf_.clear();
}

Lsw2WirelessRelay::~Lsw2WirelessRelay() { stop(); }
