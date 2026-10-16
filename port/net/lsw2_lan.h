/* LAN discovery - LAN game support by checking the player's network for any
 * games running on it, and the setting behind the host menu's
 * "Visible to LAN <YES/NO>" row.
 *
 * THIS WAS MISSING ENTIRELY. An earlier status note described the multiplayer
 * work as "transport, lockstep, master client, menus all built", which was
 * wrong twice over: LAN discovery and automatic port forwarding were both
 * required and neither had any code. The menu row existed and did nothing,
 * which is the built-and-unconsumed pattern worth always checking for.
 *
 * WHY UDP BROADCAST RATHER THAN MULTICAST. A DS advertising itself over
 * wireless is the behaviour being replaced, and the job is "checking the
 * player's network" - one subnet, no router configuration, no
 * group membership to leak when a process dies. Broadcast is what every
 * LAN-game browser of this era used and it needs nothing of the network.
 *
 * THE HOST ANNOUNCES; CLIENTS ONLY LISTEN. The alternative - clients probe and
 * hosts reply - halves idle traffic but needs the host to hold a socket open
 * for requests anyway, and it makes a client that joins the network late wait
 * for its own probe instead of hearing the next announcement. At one small
 * datagram a second per host this costs nothing worth optimising.
 *
 * NOT AUTHENTICATED, DELIBERATELY. Anything on the subnet can announce a lobby,
 * exactly as anything on the subnet could pretend to be a DS. The datagram
 * carries no secret and grants nothing: it is an invitation to try connecting,
 * and the connection is where trust would be established. The one hardening
 * that matters is that a hostile announcement cannot corrupt the listener, so
 * every field is bounds-checked and the name goes through lsw2_sanitise_name.
 */

#ifndef LSW2_LAN_H
#define LSW2_LAN_H

#include "lsw2_master.h"   /* Lsw2ServerEntry - one shape for both sources */

#include <cstdint>
#include <string>
#include <vector>

/* The advertisement port. Distinct from the game's own listen port so a host
 * and a browser can run on one machine, which is how this gets tested. */
constexpr uint16_t kLsw2LanPort = 28554;

/* "LSW2" plus a format byte. A version in the magic rather than beside it means
 * a future format change is invisible to this one instead of being a datagram
 * it half-understands. */
constexpr uint32_t kLsw2LanMagic = 0x324C5731u;   /* '1WL2' little-endian */

/* Names longer than this are cut. The wire record is fixed-size for the same
 * reason the TCP one is: there is no length field to disagree with reality. */
constexpr int kLsw2LanNameBytes = 24;
constexpr int kLsw2LanRecordBytes = 4 + 2 + 2 + 2 + 2 + 2 + kLsw2LanNameBytes;

/* THE INSTANCE ID, and why the record needs one.
 *
 * The beacon sends every announcement TWICE - to the broadcast address and to
 * loopback - so that a host and a browser on one machine find each other even
 * though most stacks do not loop a broadcast back to the sender. On a machine
 * that has both, both copies arrive, from two different source addresses, and a
 * browser keyed on (address, port) lists one host twice.
 *
 * That is not a test artifact; it is what a player on a real LAN would see.
 *
 * The id is the beacon's own bound source port, which is unique per socket on a
 * machine, so the two copies of one announcement carry the same id and merge.
 * Records from another machine cannot be confused with them because a remote
 * host's datagrams never arrive with a loopback source. */

/* THE HOST'S BEACON. Open it when a lobby starts and the host chose "Visible to
 * LAN"; destroying it stops the announcements. */
class Lsw2LanBeacon {
public:
    ~Lsw2LanBeacon();

    /* `game_port` is the port joiners should connect to - the transport's, not
     * this one's. Returns false if the socket could not be opened, which on a
     * machine with no network is normal and not fatal to hosting. */
    bool start(const std::string& name, uint16_t game_port);
    void stop();
    bool running() const { return sock_ != -1; }

    /* Call once a frame. Sends at most one datagram a second; the caller does
     * not have to rate-limit. `players` is folded in so the browser's count is
     * current without a second mechanism. */
    void tick(int players, int max_players = 4);

    /* Send now regardless of the timer. Used by the test, and by shutdown to
     * make the last announcement carry a zero. */
    bool announce(int players, int max_players);

private:
    long long sock_ = -1;
    std::string name_;
    uint16_t game_port_ = 0;
    uint16_t id_ = 0;         /* our bound source port; see above */
public:
    /* THIS BEACON'S INSTANCE ID, so a browser on the same machine can tell our
     * announcement apart from everybody else's. A host broadcasts to the subnet,
     * which includes itself, so without this a host running both would list its
     * own lobby - and with two copies on one machine for testing, each would see
     * two rows where there is one other game. */
    uint16_t id() const { return id_; }
private:
    int ticks_ = 0;
};

/* THE BROWSER. Binds the advertisement port and collects what it hears. */
class Lsw2LanBrowser {
public:
    ~Lsw2LanBrowser();

    bool start();
    void stop();
    bool running() const { return sock_ != -1; }

    /* Ignore one instance id - our own beacon's. Set it and our announcement
     * stops appearing in our own list. Zero means "ignore nothing", which is
     * also what a browser with no beacon wants. */
    void ignore(uint16_t instance_id) { ignore_ = instance_id; }

    /* Drain the socket. Records are merged by instance id, so a host
     * announcing every second updates its row rather than adding one - and the
     * two copies of one announcement collapse into a single entry. */
    void poll();

    const std::vector<Lsw2ServerEntry>& servers() const { return servers_; }

    /* Drop rows nothing has been heard from for `missed_polls` consecutive
     * polls. A host that quits without saying so otherwise stays in the list
     * for ever.
     *
     * POLLS, NOT SECONDS, and the difference is why this went unused for so
     * long. The browser counts poll() calls - it has no clock - so the first
     * version converted seconds at a fixed 60 polls per second, documenting
     * that "the browser is polled once a frame". Nothing polls it once a
     * frame: refresh_lobbies() polls once per LIST REFRESH, on demand, because
     * a socket bound for a screen nobody has opened is a socket held for
     * nothing. Under that caller a 60x multiplier makes even expire(1)
     * unreachable, so the one call site that would have used it never existed
     * and stale hosts stayed listed for ever.
     *
     * Counting the unit the browser actually has removes the mismatch instead
     * of tuning around it. */
    void expire(int missed_polls);

private:
    long long sock_ = -1;
    std::vector<Lsw2ServerEntry> servers_;
    std::vector<int> last_seen_;   /* poll() calls since each row was heard */
    uint16_t ignore_ = 0;
    std::vector<uint32_t> ids_;    /* (id << 16) | game port, the merge key   */
};

#endif /* LSW2_LAN_H */
