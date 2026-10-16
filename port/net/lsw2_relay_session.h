/* THE FORK'S OWN WIRELESS RELAY, STARTED IN-PROCESS.
 *
 * The cartridge's own wireless was proven to run a full deathmatch over a
 * UDP-over-TCP relay, using tools/wireless_bridge.py - an external script a
 * player would have to launch by hand. lsw2_wireless_relay.{h,cpp} is that
 * relay as host-layer C++ (the byte mover, polled once a frame, no threads).
 * This is the piece that stands the relay up inside the fork: it owns the TCP
 * LINK to the peer (which the python bridge served on its own port) and a
 * Lsw2WirelessRelay bound to the loopback radio, so the fork itself carries the
 * DS radio across a network with no external process.
 *
 * TWO WAYS IN. The fork's directory / room-code / direct-IP connect UI
 * resolves the peer address, agrees the seats and starts the relay with
 * explicit parameters (lsw2_relay_session_start_explicit below). The
 * environment path is the transport under it, turned on exactly the way test
 * runs turned on the python bridge, so the SAME two-instance, two-base,
 * one-machine run that proved the script also proves the fork's own native
 * relay - the external bridge removed from the loop. Absent the env and the
 * connect UI, this does NOTHING: an ordinary launch never binds a socket here.
 *
 * THE TOPOLOGY, matching tools/wireless_bridge.py exactly:
 *   LSW2_RELAY        = "host"  (listen for the joiner's link) or
 *                       "connect" (dial the host's link). Unset = off.
 *   LSW2_RELAY_LINK_PORT = TCP port of the link (default 55000).
 *   LSW2_RELAY_HOST   = address to dial in "connect" mode (default 127.0.0.1).
 *   LSW2_RELAY_BASE   = the wireless base port, == --local-wireless-port.
 *   LSW2_RELAY_LOCAL  = this instance's index,  == --instance-index.
 *   LSW2_RELAY_REMOTE = the peer's instance index.
 * The relay then catches base+remote (frames this instance sends the peer) and
 * injects base+local (the port this instance listens on). Host is local 0.
 *
 * OWNED, NOT LEAKED. The free functions below hold one session behind a
 * file-static pointer; the link and the two UDP sockets go with it. No
 * subprocess, so it cannot orphan the way the test runs' python bridges did
 * (leaked bridge processes accumulated across runs). The class is exposed so a test can stand
 * two sessions up in one process without touching the env or the singleton.
 *
 * SERVICED ON ITS OWN THREAD once the link is up, and this is not a detail.
 * The guest's WH admission BLOCKS the frame loop (a child waits up to ~60 s for
 * the host's dataset). If the relay were pumped from that same loop, it would
 * stop injecting the host's frames exactly while the child is blocked waiting
 * for them - a deadlock that timed the child out with WH error 5.
 * The python bridge never hit this because it ran in a separate process,
 * delivering frames continuously. A dedicated thread here gives the same
 * continuous delivery: it pumps the relay while the frame loop is blocked, so
 * the injected frames reach the runner's UDP socket and the guest's wait
 * completes. The thread touches only the relay's own sockets and the link; it
 * is joined before the sockets close.
 */

#ifndef LSW2_RELAY_SESSION_H
#define LSW2_RELAY_SESSION_H

#include "lsw2_wireless_relay.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct Lsw2RelaySessionStatus {
    const char* phase;              /* off | listening | connecting | linked | closed */
    unsigned long long forwarded;   /* loopback radio -> link */
    unsigned long long injected;    /* link -> loopback radio */
};

class Lsw2RelaySession {
public:
    enum class Phase { Off, Listening, Connecting, Linked, Closed };

    Lsw2RelaySession() = default;
    ~Lsw2RelaySession();
    Lsw2RelaySession(const Lsw2RelaySession&) = delete;
    Lsw2RelaySession& operator=(const Lsw2RelaySession&) = delete;

    /* Read the LSW2_RELAY* env and, if set, begin bringing up the link. Returns
     * false (and leaves the session Off) if the env asks for no relay. */
    bool start_from_env();

    /* The same, with explicit parameters - what start_from_env() calls once it
     * has parsed the env, and what a test uses to avoid the env and singleton. */
    bool start(bool is_host, uint16_t link_port, const std::string& host_addr,
               int base, int local, int remote);

    /* Advance the link setup and notice a closed link; call once a frame. While
     * Listening it accepts the joiner's link, while Connecting it finishes the
     * connect, and once Linked it only checks whether the service THREAD has
     * seen the link close (the thread does the actual byte pumping). */
    void service();

    /* Close the link and UDP sockets. Idempotent. */
    void stop();

    Phase phase() const { return phase_; }
    Lsw2RelaySessionStatus status() const;

private:
    bool begin_listen(const char* detail);
    bool begin_connect(const char* detail);
    void poll_accept();
    void poll_connect();
    void arm_relay();

    Phase phase_ = Phase::Off;
    bool is_host_ = false;
    uint16_t link_port_ = 55000;
    uint16_t catch_port_ = 0;
    uint16_t inject_port_ = 0;
    std::string host_addr_ = "127.0.0.1";
    /* Socket handles kept as uintptr_t so the header stays free of winsock; the
     * .cpp casts to the platform socket type. ~0 is "none". */
    uintptr_t listen_ = ~uintptr_t(0);
    uintptr_t link_ = ~uintptr_t(0);
    Lsw2WirelessRelay relay_;

    /* The pump thread, live only while Linked. `stop_` asks it to exit;
     * `link_closed_` is how it reports the link went away, read by service(). */
    std::thread pump_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> link_closed_{false};
    void pump_loop();
    void join_pump();
};

/* The singleton the shell drives. start_from_env is safe to call every frame -
 * it acts only the first time. */
bool lsw2_relay_session_start_from_env();
void lsw2_relay_session_service();
void lsw2_relay_session_stop();
Lsw2RelaySessionStatus lsw2_relay_session_status();

/* Start the singleton with EXPLICIT params, the way the fork's connect UI
 * does: the session knows its role, its AID (== local), the peer's AID
 * (== remote), the peer address, and the wireless base - so it drives the relay
 * itself instead of the player setting LSW2_RELAY*. A no-op (returns true) if the
 * env path already brought a relay up, so LSW2_RELAY still wins if both are set.
 * Idempotent per singleton: a second call while Linked leaves the link alone. */
bool lsw2_relay_session_start_explicit(bool is_host, uint16_t link_port,
                                       const char* host_addr, int base,
                                       int local, int remote);

#endif /* LSW2_RELAY_SESSION_H */
