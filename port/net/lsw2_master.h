/* The master-server client.
 *
 * Talks to services/master-server-selfhost over plain HTTP. That service is the
 * contract; this is the other end of it, and the two are tested against each
 * other rather than against a mock.
 *
 * BLOCKING CALLS, DELIBERATELY, AND NOT FROM THE FRAME LOOP. Every method here
 * waits on a socket. The port runs a 60 Hz loop with a hard frame budget, so
 * calling `refresh()` from it would drop frames the moment the network hiccups.
 * These belong on a worker thread, or at a moment the player is already waiting
 * - opening the server list, pressing Start Lobby. The alternative, an async
 * state machine, buys nothing here: nothing in the multiplayer flow needs a
 * directory lookup mid-match.
 *
 * NO TLS. The service is plain HTTP and the client matches it. That is a real
 * limitation and it is stated rather than glossed: a room code and a player name
 * cross the wire in the clear, and the token that authorises updating a lobby
 * does too. For a self-hosted service among friends that is a reasonable trade;
 * for anything wider it is not, and the answer would be a reverse proxy in
 * front of the service rather than TLS bolted in here.
 */

#ifndef LSW2_MASTER_H
#define LSW2_MASTER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* The two kinds of lobby the directory holds. The fork advertises arena
 * matches; the base recomp advertises co-op games. An empty kind means "do not filter, do not declare" - which
 * the service reads as bounty, matching what a client written before the column
 * existed sends. */
#define LSW2_KIND_BOUNTY "bounty"
#define LSW2_KIND_COOP   "coop"

struct Lsw2ServerEntry {
    std::string name;
    std::string address;
    std::string code;
    /* The lobby's id in the directory: what a joiner names to reach it
     * through the relay (lsw2_wsrelay.h) when its address does not answer.
     * Empty from a directory without a relay, or for a LAN find. */
    std::string id;
    std::string kind;
    int port = 0;
    int players = 0;
    int max_players = 4;
    /* MEASURED BY THIS CLIENT, not supplied by the directory. A ping is between
     * you and the host; a number from the master server would be the distance
     * to the master server. -1 until measured. */
    int ping_ms = -1;
    /* Names only. The directory does not publish player addresses on the public
     * list, and a client has no use for one - it connects to the host. */
    std::vector<std::string> roster;

    /* FOUND ON THE LOCAL NETWORK RATHER THAN IN THE DIRECTORY.
     *
     * The base recomp's co-op lobby list needs this and nothing carried it:
     * the list merges LAN games with the master server's entries, putting
     * LAN games first and highlighting them in green. Before this field
     * a LAN entry was appended to the directory's rows and became
     * indistinguishable from them, so neither the ordering nor the colour
     * could be expressed.
     *
     * The fork deliberately does NOT reorder or recolour - its LAN games are
     * mixed into the list - so this is a fact the entry carries, not a
     * presentation it forces. */
    bool lan = false;
};

/* What a host tells the directory about who is in its lobby. The address is
 * the host's own observation of the peer's socket; it exists so an operator's
 * Ban IP has something to act on, and nothing authorises off it. */
struct Lsw2RosterEntry {
    int slot = 0;
    std::string name;
    std::string address;
};

/* An instruction from the directory's operator, delivered on a heartbeat reply.
 * Today the only action is "kick". */
struct Lsw2Directive {
    std::string action;
    std::string address;
    int slot = -1;
};

class Lsw2MasterClient {
public:
    void configure(const std::string& host, int port);
    const std::string& host() const { return host_; }
    int port() const { return port_; }

    /* Which kind of lobby this client advertises and lists. Set once at start
     * up: the fork sets bounty, the base recomp sets coop. */
    void set_kind(const std::string& kind) { kind_ = kind; }
    const std::string& kind() const { return kind_; }

    /* Who is in the lobby, sent with the next register or heartbeat. Set it
     * whenever the seat table changes; it costs nothing until the next beat. */
    void set_roster(const std::vector<Lsw2RosterEntry>& roster);

    /* What the lobby is doing ("lobby" or "match") and its rules as a JSON
     * object, sent with the next register or heartbeat - so the directory can
     * say when a game starts and with what settings. Empty config sends none. */
    void set_status(const std::string& state, const std::string& config_json) {
        state_ = state;
        config_ = config_json;
    }

    /* Instructions the directory's operator has queued for this host, collected
     * from heartbeat replies. Draining them is the caller's job - they arrive
     * once and the service will not repeat them. */
    std::vector<Lsw2Directive> take_directives();

    /* True when the service answered /v1/health. Cheap, and the only honest way
     * to tell "misconfigured" from "nobody is hosting". */
    bool reachable();

    /* Advertise a lobby. `listen_port` is the port the GAME listens on, not the
     * one this request came from - the service takes the address from the
     * socket but cannot know the game's port. */
    bool register_lobby(const std::string& name, int listen_port, int players);
    bool heartbeat(int players);
    /* THE SAME BEAT, OFF THE FRAME THREAD. A synchronous beat costs a DNS
     * lookup, a TCP handshake and an HTTP round trip to the directory: about
     * a millisecond to a server on this machine, but 100-300 ms to the hosted
     * one - and for that long the host's frame does not advance, so every
     * joiner's lockstep holds too. This snapshots the request, runs it on a
     * worker, and returns at once; directives in the reply arrive through
     * take_directives() on a later frame. A beat still in flight makes the
     * next call a no-op (returns false) rather than queueing behind it. */
    bool heartbeat_async(int players);
    bool unregister();

    bool refresh();                       /* fills servers() */
    const std::vector<Lsw2ServerEntry>& servers() const { return servers_; }

    /* Resolve a room code. Returns false when the code is unknown, which is a
     * different answer from the service being unreachable - callers should say
     * which. */
    bool resolve(const std::string& code, Lsw2ServerEntry* out);

    const std::string& room_code() const { return code_; }
    /* This host's registration - the relay authenticates the host's own
     * connection with them. Empty until register_lobby succeeds. */
    const std::string& lobby_id() const { return id_; }
    const std::string& lobby_token() const { return token_; }
    const std::string& last_error() const { return error_; }

private:
    bool request(const char* method, const std::string& path,
                 const std::string& body, std::string* response);

    std::string host_ = "lsw2server.viosarcade.xyz";
    int port_ = 80;
    std::string id_, token_, code_, error_, kind_;
    std::string state_, config_;
    std::string status_json() const;
    std::vector<Lsw2ServerEntry> servers_;
    std::vector<Lsw2RosterEntry> roster_;
    std::vector<Lsw2Directive> directives_;

    /* What an asynchronous beat shares with its worker. Held by shared_ptr so
     * a worker still running when the client is destroyed writes into memory
     * that is still alive. */
    struct AsyncBeat {
        std::mutex lock;
        std::vector<Lsw2Directive> directives;
        std::string error;
        std::atomic<bool> busy{false};
    };
    std::shared_ptr<AsyncBeat> async_ = std::make_shared<AsyncBeat>();
};

/* Exposed for testing: the flat-JSON readers this client uses. The responses
 * are one object of scalars, or one array of them, so a full parser would be
 * more code than the thing it parses. */
std::string lsw2_json_string(const std::string& json, const char* key);
long long lsw2_json_number(const std::string& json, const char* key,
                           long long fallback);

/* Split the top-level objects out of an array. Pass the array's key, or an
 * empty string to take every top-level object in the text.
 *
 * THIS REPLACED A KEY-NAME WALK. Finding each "name" and reading the fields
 * after it worked until the directory started reporting rosters, at which point
 * a player's "name" manufactured a phantom server, which then took its port
 * from the following real server. Depth counting cannot be fooled that way. */
std::vector<std::string> lsw2_json_objects(const std::string& json,
                                           const char* array_key);

#endif /* LSW2_MASTER_H */
