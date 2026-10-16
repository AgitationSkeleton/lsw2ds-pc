/* Round-trip time to a lobby host.
 *
 * The browser row is the player's name followed by the player count out of 4,
 * the PING NUMBER and the DS signal-strength icon indicating connection
 * strength, e.g. "Maclunkey 1/4 96 <green DS icon>".
 *
 * `Lsw2ServerEntry::ping_ms` has existed since the master client was written,
 * with a comment saying it is "MEASURED BY THIS CLIENT, not supplied by the
 * directory" and "-1 until measured". Nothing measured it. The list formatted
 * it, the signal thresholds classified it, and every row showed no number and no
 * icon - a whole column that read as "the directory did not say" when the truth
 * was "nobody asked".
 *
 * WHAT IS MEASURED IS A TCP CONNECT, and that is deliberate rather than
 * convenient. There is no ICMP here and no UDP echo on the host: the only thing
 * a lobby host is guaranteed to answer is the port it is listening on. Timing
 * the connect measures the path the JOIN will take, over the protocol the join
 * will use, through whatever NAT and UPnP mapping sits in between. A number that
 * came from anything else would be a different question's answer.
 *
 * The connect is closed immediately. A host sees an accept and a hangup, which
 * its transport already handles - a peer that connects and vanishes is the same
 * shape as one that crashes during the handshake.
 *
 * ALL AT ONCE, NOT ONE AFTER ANOTHER. The browser opens after a directory
 * refresh the player has already waited on; a serial probe of eleven hosts at a
 * second each would add eleven more. Every connect is started, then one select
 * waits for all of them, so the whole sweep costs the timeout once.
 */

#ifndef LSW2_PING_H
#define LSW2_PING_H

#include <string>
#include <vector>

/* One host to probe, and where its answer goes. */
struct Lsw2PingTarget {
    std::string address;
    int port = 0;
    /* Milliseconds, or -1 for "did not answer in time". NOT zero: zero is a
     * legitimate sub-millisecond result on a loopback or a LAN, and a caller
     * that treated failure as zero would draw a full-strength icon for a host
     * nobody can reach. */
    int ping_ms = -1;
};

/* Probe every target. Returns how many answered.
 *
 * `budget_ms` bounds the WHOLE sweep, not each target. Anything still
 * outstanding when it expires is left at -1, which the signal thresholds
 * already read as "unmeasured" rather than "bad". */
int lsw2_ping_sweep(std::vector<Lsw2PingTarget>& targets, int budget_ms);

/* One target, same rules. Exposed because a single probe is what the join path
 * wants and because it is the easier thing to test. */
int lsw2_ping_one(const std::string& address, int port, int budget_ms);

#endif /* LSW2_PING_H */
