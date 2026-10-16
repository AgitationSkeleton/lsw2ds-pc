/* THE DS RADIO, CARRIED ACROSS A NETWORK - native, in the fork.
 *
 * The cartridge's own wireless was proven to run a full deathmatch over a
 * UDP-over-TCP relay (tools/wireless_bridge.py). The design that followed has
 * the fork start that relay itself, so a player never runs a python script by
 * hand. This is the design's FIRST BUILDABLE PIECE: the relay as
 * host-layer C++, the same shape as lsw2_tcp.cpp - polled once a frame, no
 * threads, deterministic to test.
 *
 * WHAT IT DOES. The framework emulates the radio as loopback UDP between local
 * instances (runner/src/wifi_net.cpp LocalMpTransport): each instance binds
 * 127.0.0.1:(base + ownIndex) and sends every frame to 127.0.0.1:(base + i) for
 * other instances. On one machine that is all any peer needs; across a network
 * the datagrams never leave loopback. This relay:
 *
 *   - CATCHES the frames the local instance sends to the REMOTE peer's slot
 *     (it binds 127.0.0.1:catch_port = base + remoteIndex) and forwards each,
 *     length-prefixed, over the TCP link;
 *   - INJECTS each datagram from the link into 127.0.0.1:inject_port =
 *     base + localIndex, the port the local instance is listening on, so the
 *     instance receives it as a local peer would. The datagram's own sender
 *     field is untouched, so the cartridge sees the real peer index.
 *
 * The frame is opaque - the relay moves bytes, it does not parse the NIFI
 * header. Ordering is TCP's, stronger than the UDP the radio assumed.
 *
 * OWNED BY THE SESSION, so it cannot leak the way the test runs' python relays
 * did: one object, started with the connection and stopped when the
 * session ends - no globals, no subprocess.
 */

#ifndef LSW2_WIRELESS_RELAY_H
#define LSW2_WIRELESS_RELAY_H

#include <cstdint>
#include <vector>

class Lsw2WirelessRelay {
public:
    Lsw2WirelessRelay() = default;
    ~Lsw2WirelessRelay();

    Lsw2WirelessRelay(const Lsw2WirelessRelay&) = delete;
    Lsw2WirelessRelay& operator=(const Lsw2WirelessRelay&) = delete;

    /* Bind the two loopback UDP sockets. `catch_port` is base + the peer's
     * index (frames headed for the peer land here); `inject_port` is base +
     * this machine's index (frames from the peer are delivered here). The TCP
     * link is handed in already connected - the fork's own join/host path has
     * one to the peer. Returns false if either socket cannot bind. */
    bool start(uint16_t catch_port, uint16_t inject_port, uintptr_t link_socket);

    /* Pump both directions, non-blocking. Call once a frame, like
     * Lsw2TcpTransport::service. Returns false once the TCP link has closed, so
     * the caller can tear the session down. */
    bool service();

    /* Close the UDP sockets. The link socket is NOT closed here - the caller
     * owns it (it is the session's transport link). */
    void stop();

    bool running() const { return running_; }
    unsigned long long forwarded() const { return tx_; }   /* UDP -> link */
    unsigned long long injected() const { return rx_; }    /* link -> UDP */

    /* THE PURE FRAMING, split out so a test can pin it without sockets. A datagram
     * goes on the wire as a 4-byte little-endian length then its bytes; this
     * appends `payload` to `out` in that form. */
    static void frame(const uint8_t* payload, int len, std::vector<uint8_t>& out);

    /* Pull as many whole datagrams as `buf` holds off the front, appending each
     * to `out` (one vector per datagram), and erase what was consumed. A partial
     * trailing datagram is left in `buf` for next time. Returns how many whole
     * datagrams it delivered. */
    static int deframe(std::vector<uint8_t>& buf, std::vector<std::vector<uint8_t>>& out);

private:
    bool running_ = false;
    uint16_t inject_port_ = 0;
    uintptr_t catch_sock_ = ~uintptr_t(0);
    uintptr_t inject_sock_ = ~uintptr_t(0);
    uintptr_t link_ = ~uintptr_t(0);
    std::vector<uint8_t> link_buf_;   /* partial datagram from the TCP link */
    unsigned long long tx_ = 0;
    unsigned long long rx_ = 0;
};

#endif /* LSW2_WIRELESS_RELAY_H */
