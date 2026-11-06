/* ONLINE PLAY WITH NO PORT FORWARDING: the game's TCP stream carried over the
 * master server's relay (services/master-server/src/relay.js).
 *
 * WHY. A host has to accept incoming connections, and many cannot - a router
 * with UPnP off or absent, an ISP's carrier-grade NAT, a VPN whose exit address
 * accepts nothing. The lobby was listed at an address that never answered, and
 * the joiner's game hung on the connect. Through the relay both sides dial OUT
 * to the master server, which works from anywhere a web page loads.
 *
 * THE GAME'S TRANSPORT DOES NOT CHANGE. Lsw2TcpTransport still speaks TCP to a
 * socket; the relay only decides which socket:
 *
 *   joiner  Lsw2RelayJoin listens on 127.0.0.1:<ephemeral>; the transport joins
 *           THAT, and the bridge carries the bytes to the relay room.
 *   host    Lsw2RelayHost holds one connection to the room; each joiner the
 *           room announces becomes a loopback connection to the host's own
 *           listen port, which its transport accepts like any other.
 *
 * So the lockstep, the records and every guest-facing piece are exactly what a
 * direct connection runs; a test of one is a test of the other.
 *
 * THE WIRE to the room is WebSocket (RFC 6455) over plain HTTP on the master
 * server's port - the same host and port the lobby list already uses, so a
 * network that lets the list load lets the relay through. Binary messages:
 * the host's carry [kind][peer][bytes] (0 data, 1 open, 2 close); a joiner's
 * are the bytes alone. See relay.js.
 *
 * THREADS. Each bridge pumps on its own thread with select(), so the relay
 * keeps moving while the frame loop is busy or blocked. The threads touch only
 * their own sockets; the transport reaches them through loopback TCP. */

#ifndef LSW2_WSRELAY_H
#define LSW2_WSRELAY_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ------------------------------------------------------------- the framing
 * Pure functions, so the codec is testable without a network. */

/* A client frame: FIN, `opcode`, masked with `mask` (clients must mask). */
std::string lsw2_ws_frame(uint8_t opcode, const uint8_t* data, size_t n,
                          uint32_t mask);

struct Lsw2WsMessage {
    uint8_t opcode = 0;        /* 1 text, 2 binary, 8 close, 9 ping, 10 pong */
    std::string data;
};

/* Take whole frames off the front of `buf` (a server's, possibly masked) and
 * append the messages they complete. Continuation frames are joined. Returns
 * false on a malformed stream. */
bool lsw2_ws_parse(std::string& buf, std::string& partial, uint8_t& partial_op,
                   std::vector<Lsw2WsMessage>* out);

/* ------------------------------------------------------------- the client */

class Lsw2WsClient {
public:
    ~Lsw2WsClient() { close(); }

    /* Connect to host:port and upgrade `path`. `token` goes as X-Token when
     * set. Blocking, bounded by `timeout_ms` overall. */
    bool open(const std::string& host, int port, const std::string& path,
              const std::string& token, int timeout_ms, std::string* error);

    /* Safe from any thread: sends are serialised by a lock. */
    bool send_binary(const uint8_t* data, size_t n);
    bool send_text(const std::string& text);

    /* Read what is waiting (call after select says readable) and parse it.
     * Pings are answered here. False once the connection is closed. ONE
     * thread receives. */
    bool receive(std::vector<Lsw2WsMessage>* out);

    /* Messages that arrived in the same read as the upgrade's response - the
     * room's "ready" can. Call once after open(), before waiting on the socket:
     * select will not report bytes that have already been read. */
    void take_early(std::vector<Lsw2WsMessage>* out) {
        for (Lsw2WsMessage& m : early_) out->push_back(std::move(m));
        early_.clear();
    }

    /* The close code and reason the server last sent, for the player. */
    const std::string& close_reason() const { return close_reason_; }

    void close();
    bool is_open() const { return fd_.load() >= 0; }
    long long fd() const { return fd_.load(); }

private:
    bool send_frame(uint8_t opcode, const uint8_t* data, size_t n);

    std::atomic<long long> fd_{-1};
    std::mutex send_mu_;
    std::vector<Lsw2WsMessage> early_;
    std::string buf_, partial_, close_reason_;
    uint8_t partial_op_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
};

/* ------------------------------------------------------------- the stream
 *
 * ONE ORDERED STREAM OVER SEVERAL LANES. Every chunk is sent on every lane as
 * [seq u32 LE][bytes]; the receiver delivers each sequence number once, in
 * order, from whichever lane brought it first, and drops the copies. A lane
 * that stalls - a lost packet holds a TCP stream for a retransmit timeout -
 * costs nothing while another lane is moving. */
class Lsw2LaneStream {
public:
    /* The wire form of the next outgoing chunk. */
    std::string wrap(const char* data, size_t n);
    /* A chunk off any lane: append whatever is now deliverable, in order, to
     * `out`. False for a malformed chunk. */
    bool take(const std::string& chunk, std::string* out);
    /* Chunks held waiting for an earlier one (for the log). */
    size_t waiting() const { return early_.size(); }

private:
    uint32_t send_seq_ = 0;
    uint32_t next_ = 0;
    std::map<uint32_t, std::string> early_;
};

/* How many lanes each side holds: LSW2_RELAY_LANES (1..4), default 2. */
int lsw2_relay_lanes();

/* ------------------------------------------------------------- the bridges */

/* THE HOST'S END: keeps every lane connected (each reconnecting on its own
 * thread) and turns each relayed joiner into a loopback connection to
 * `listen_port`. */
class Lsw2RelayHost {
public:
    Lsw2RelayHost(std::string master_host, int master_port, std::string id,
                  std::string token, uint16_t listen_port);
    ~Lsw2RelayHost();

    void start();
    void stop();

    /* True while any lane is connected - joiners can reach this host. */
    bool online() const { return lanes_up_.load() > 0; }
    /* How many joiners the relay is carrying right now. */
    int peers() const { return peers_.load(); }

private:
    struct Peer;
    void lane_run(int lane);
    void local_run();
    void handle(int lane, const std::string& msg);
    void drop(int n, bool tell);
    void send_all_lanes(const std::string& msg);

    std::string host_, id_, token_;
    int port_;
    uint16_t listen_port_;
    int lanes_n_ = 2;
    std::atomic<bool> stop_{false};
    std::atomic<int> lanes_up_{0};
    std::atomic<int> peers_{0};
    std::vector<std::unique_ptr<Lsw2WsClient>> lanes_;
    std::vector<std::thread> threads_;
    std::mutex mu_;                       /* peers_map_ */
    std::map<int, std::unique_ptr<Peer>> peers_map_;
};

/* THE JOINER'S END: one stream from a loopback listener to the room, over
 * every lane. */
class Lsw2RelayJoin {
public:
    ~Lsw2RelayJoin() { stop(); }

    /* Connect to the lobby's room and wait for it to say the host is there,
     * then listen on loopback. False (with a reason) when the relay cannot
     * reach the lobby. Blocking, bounded by `timeout_ms`. */
    bool start(const std::string& master_host, int master_port,
               const std::string& id, int timeout_ms, std::string* error);

    /* Where the transport should join: 127.0.0.1:<this>. */
    uint16_t local_port() const { return local_port_; }

    /* False once the stream has ended (the host left, or every lane was lost
     * for too long). */
    bool alive() const { return alive_.load(); }

    void stop();

private:
    void lane_run(int lane);
    void local_run();
    void deliver(const std::string& bytes);   /* under mu_ */
    void send_all_lanes(const std::string& msg);
    std::string lane_path(int lane) const;

    std::string host_, id_, link_;
    int port_ = 80;
    int lanes_n_ = 2;
    std::vector<std::unique_ptr<Lsw2WsClient>> lanes_;
    std::vector<std::thread> threads_;
    long long listen_fd_ = -1;
    std::atomic<long long> local_fd_{-1};
    uint16_t local_port_ = 0;
    std::mutex mu_;                /* stream_, pending_, the local socket's writes */
    Lsw2LaneStream stream_;
    std::string pending_;          /* bytes before the transport connected */
    std::atomic<bool> stop_{false};
    std::atomic<bool> alive_{false};
    std::atomic<bool> ended_{false};
};

#endif /* LSW2_WSRELAY_H */
