/* A TCP transport for the lockstep session.
 *
 * FIXED-SIZE RECORDS, AND WHY THAT MATTERS. Every message on this wire is
 * exactly the same size:
 *
 *     u32 frame | u8 station | u8 flags | u16 pad | 124 bytes of block
 *
 * 132 bytes, always. That is a direct consequence of WM DataSharing having a
 * constant `dataLength` - the payload genuinely never varies - and it removes
 * the entire class of bugs that length-prefixed framing brings with it: no
 * partial-header parsing, no length field to disagree with reality, no
 * attacker-chosen allocation. Read 132 bytes, you have a message.
 *
 * A STAR, NOT A MESH. The host relays: each client sends only to the host, and
 * the host broadcasts. With four stations a mesh would be six connections
 * instead of three, and every one of them a second NAT traversal problem.
 * Multiplayer here is host-and-joiners anyway - people join someone's lobby.
 *
 * The cost is honest and worth stating: the host's upstream carries three
 * copies of every frame, and a client's latency to another client is two hops.
 * At 124 bytes and 60 Hz that is about 30 KB/s up on the host, which is
 * nothing, and the second hop is why the lockstep delay is measured rather than
 * assumed.
 */

#ifndef LSW2_TCP_H
#define LSW2_TCP_H

#include "lsw2_lockstep.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

/* One wire record. Packed by hand rather than memcpy'd from a struct, because a
 * struct's layout is the compiler's business and this is a wire format. */
constexpr int kLsw2RecordBytes = 8 + kLsw2ShareBytes;   /* 132 */

/* The host-level WH's control subtypes (lsw2_vwh.h), carried on flags bit 7. */
constexpr uint8_t kLsw2VwhCommit = 1;
constexpr uint8_t kLsw2VwhStart = 2;
/* A round-trip probe: the host sends it with `frame` = a token, a
 * client echoes the same record back. The only control record a host takes from
 * a client, and it carries nothing but the token. */
constexpr uint8_t kLsw2VwhPing = 3;

class Lsw2TcpTransport : public Lsw2NetTransport {
public:
    Lsw2TcpTransport();
    ~Lsw2TcpTransport() override;

    /* Listen for up to kLsw2MaxStations-1 joiners. Returns the bound port, or
     * 0 on failure; pass 0 to let the OS choose, which is what the tests do. */
    uint16_t host(uint16_t port);

    /* Connect to a host. `station` is assigned by the host and reported back
     * through assigned_station() once the handshake completes. */
    /* `timeout_ms` bounds the connect; a join with the relay to fall back on
     * tries the direct route for less (see forksession.cpp, join_address). */
    bool join(const std::string& address, uint16_t port, int timeout_ms = 3000);

    /* Pump sockets. Call once per frame BEFORE the session's step(), so a block
     * that arrived since the last frame is available to it. */
    void service();

    /* Wait up to `timeout_us` for bytes from a peer (or, hosting, a joiner at
     * the door). True if something is readable. For the host-level WH, which
     * holds emulation for the milliseconds a dataset is in flight rather than
     * let the game see it missing (port/frontend/vwh.cpp). */
    bool wait_readable(int timeout_us);

    /* HOST SETTINGS, HOST -> CLIENT, out of band of the lockstep.
     *
     * The host's Extras reach each client as it joins, and the host has authority over customization. Neither is a per-frame
     * block, so neither belongs in the lockstep's share - a settings change is
     * rare and must arrive whether or not a round is running.
     *
     * The record framing already had room: byte 5 is a flags byte and only bit 0
     * was used, for the seat assignment. Bit 1 is this. A record carrying it has
     * no block and must never reach the session as one, exactly as the seat
     * assignment does not.
     *
     * `broadcast_settings` sends to every seated peer; `take_settings` hands the
     * client the most recent record it received, or false if none has arrived. */
    void broadcast_settings(const uint8_t* payload, int n);
    bool take_settings(uint8_t* out, int n);

    /* WHO EACH STATION IS PLAYING AS, carried on every block.
     *
     * The record's 8-byte header is the PORT's - frame, station, flags, and two
     * bytes nobody used. The 124-byte share after it is the GUEST's
     * datasharing block and is not ours to extend: "a different size is a
     * different protocol and the guest would reject it".
     *
     * So byte 6 carries the sender's character id. Every block already says
     * WHICH station sent it; now it also says WHAT that station is. That costs
     * nothing - the bytes were being zeroed anyway - and it is what the host's
     * roster rules need to agree across machines, and what remote avatars in the lobby
     * will read to know which model to draw.
     *
     * 0xFF means "not stated", which is what a peer that has not set one sends
     * and what an unseated station reads back. */
    /* CUSTOMIZATION PARTS, both directions.
     *
     * Everyone appears in the Cantina lobby, where each player sees all the
     * others, the host included, in their own customizations - so every
     * machine needs every player's parts.
     *
     * WHY THIS IS NOW WORTH SENDING, when it was twice judged not to be. The
     * objection was that a remote player's parts had nowhere to be applied -
     * the customization block is a single global set. A per-player record was
     * found later, and then its READER: `0x02098BD4` packs each
     * of the four slots' character and five parts into a compact struct, and its
     * one caller sits in the arena state's enter. So parts written into a peer's
     * slot are picked up by the game's own code.
     *
     * FIVE HALFWORDS, not six. The global block has six slots (an earlier
     * reading that said otherwise was corrected) and the per-player record's
     * initialiser writes five; the sixth is an open question not guessed at, and
     * sending a field whose existence is unsettled would be inventing protocol.
     *
     * Flags bit 4, the same shape as the name record and for the same reasons:
     * it carries no block, it travels both ways, and the host relays with the
     * sender's true station stamped in so nobody can dress anybody else. */
    void send_parts(const uint16_t* parts, int count);
    bool take_parts(int* station, uint16_t* out, int count);

    void set_local_character(int character_id);
    int peer_character(int station) const;

    /* PLAYER NAMES, both directions.
     *
     * The join popup ("<Playername> has joined the game!") and the Kick list
     * need real player names; both have been drawing "Player 1", "Player 2" because a
     * name had nowhere to travel. A name does not fit in the header's spare
     * byte, so it gets a record of its own on flags bit 2 - the same shape as
     * the settings record, and carrying no block for the same reason.
     *
     * BOTH DIRECTIONS, unlike the settings record. A client tells the host what
     * it is called; the host relays so every client learns the others, and
     * sends its own so joiners learn the host. Everyone ends up knowing
     * everyone, which is what a four-name kick list and four join popups need.
     *
     * `take_name` yields one (station, name) at a time and returns false when
     * the queue is empty. */
    /* THE SHARED RANDOM SEED, host -> clients.
     *
     * The game's RNG is MT19937 seeded from the real-time clock at boot, so four
     * machines are GUARANTEED to hold different states - and a lockstep session
     * is only deterministic if every machine draws the same numbers in the same
     * order. No amount of correct block delivery fixes that; it is a property of
     * the simulation, not of the transport.
     *
     * One word, on flags bit 3, sent by the host. Carries no block, like the
     * other out-of-band records. */
    void send_seed(uint32_t seed);
    bool take_seed(uint32_t* out);

    /* THE MATCH-START SIGNAL, host -> clients (flags bit 5). Sent when the host
     * leaves the lobby for the arena, so a joiner idle at state 48 learns to
     * follow. Carries map/rule/seconds for the arena build. take_start_match
     * yields it once; out params may be null. */
    void send_start_match(uint8_t map, uint8_t rule, uint16_t seconds);
    bool take_start_match(uint8_t* map, uint8_t* rule, uint16_t* seconds);

    /* A RELAYED NETGAME MESSAGE record (flags bit 6). The cartridge's
     * own netgame message stream never crosses the DS-WM link (which is never
     * established), so the fork carries it: the sender walks its own station's
     * message ring and sends each new record here; the receiver dispatches it
     * in-frame. `station` is the ORIGINAL sender's station; `len` is the record
     * length in bytes (<= kLsw2ShareBytes). Host relays to other clients, like a
     * block. take_netmsg yields one at a time, false when empty. */
    void send_netmsg(int station, const uint8_t* record, int len);
    bool take_netmsg(int* station, uint8_t* out, int* len);
    /* THE HOST-LEVEL WH's CONTROL RECORDS, host -> client (flags bit 7),
     * see lsw2_vwh.h. Blocks still travel as ordinary block records - the host relays
     * each one on arrival - and these two tell the clients how to assemble them:
     *
     *   kLsw2VwhStart  (subtype 2): frame = the client's first slot, a = its AID,
     *                  b = the pipeline depth L. Sent once, to one client.
     *   kLsw2VwhCommit (subtype 1): frame = the dataset index, a = the member
     *                  bitmap of that dataset. Sent to every client, always AFTER
     *                  the blocks it names (TCP keeps a connection's order).
     *                  The 124 bytes after the header carry the host's EFFECTS
     *                  for that dataset (lsw2_vwh.h) - zeroes when there are none.
     *
     * `to_station` < 0 broadcasts. Never relayed: only the host speaks these.
     * `payload` (up to kLsw2ShareBytes, the rest zeroed) rides in the record's
     * block bytes; `take_vwh`'s `payload`, if given, receives all of them. */
    void send_vwh(int to_station, uint8_t subtype, uint32_t frame,
                  uint8_t a, uint8_t b, const uint8_t* payload = nullptr,
                  int n = 0);
    bool take_vwh(uint8_t* subtype, uint32_t* frame, uint8_t* a, uint8_t* b,
                  uint8_t* payload = nullptr);

    /* THE ROUND TRIP, for the pipeline depth each joiner is given (lsw2_vwh.h).
     * A client echoes a ping with `send_vwh_pong`; the host reads the echoes -
     * each with the station its CONNECTION belongs to - with `take_vwh_pong`. */
    void send_vwh_pong(uint32_t token);
    bool take_vwh_pong(int* station, uint32_t* token);

    void send_name(const char* name);

    /* Name a station other than this one. HOST ONLY, and it does nothing on a
     * client - naming somebody else is exactly the authority the host stamps
     * out of client records in drain().
     *
     * It exists because names are announced once, when a peer joins. The third
     * player to arrive was told the host's name and nothing else: the second
     * player's record had been relayed before the third had a socket. So on
     * each accept the host repeats the roster it already knows. */
    void send_name_as(int station, const char* name);
    bool take_name(int* station, char* out, int n);

    /* Accepted joiners, host side. -1 when nothing new. */
    int take_joined();
    /* Stations that went away since the last call. -1 when none. */
    int take_lost();

    int assigned_station() const { return assigned_; }
    bool connected() const { return connected_; }
    void close();

    /* Where a seated peer connected from, host side. Empty for an unknown
     * station, and always empty on a client - a joiner knows the host's address
     * because it dialled it, and knows nothing about the other joiners.
     *
     * Taken from accept(), never from anything a peer claims. It exists so the
     * host can report a roster the directory's operator can act on; nothing in
     * the port authorises off it. */
    std::string peer_address(int station) const;

    /* Lsw2NetTransport */
    void send(int frame, int from_station, const uint8_t* block) override;
    bool poll(int* frame, int* from_station, uint8_t* block) override;
    void disconnect(int station) override;

private:
    struct Peer;
    void relay(const uint8_t* record, int from_station);
    void drain(Peer& p);
    /* EVERY RECORD LEAVES THROUGH A PER-PEER QUEUE. The sockets are
     * non-blocking, and a bare send() on a full socket buffer writes part of a
     * record or none of it - either way the peer's framing is lost for good, as
     * every later record then starts mid-record. Harmless on a LAN, fatal on a
     * congested internet link. enqueue() appends and tries to flush; service()
     * flushes whatever is still waiting. */
    void enqueue(Peer& p, const uint8_t* record);
    void flush(Peer& p);
    struct VwhNews {
        uint8_t subtype; uint32_t frame; uint8_t a, b;
        uint8_t payload[kLsw2ShareBytes];
    };
    std::deque<VwhNews> vwh_;
    struct VwhPong { int station; uint32_t token; };
    std::deque<VwhPong> vwh_pongs_;

    bool is_host_ = false;
    bool connected_ = false;
    int assigned_ = 0;
    intptr_t listen_fd_ = -1;
    std::vector<Peer*> peers_;
    std::deque<std::vector<uint8_t>> inbox_;
    std::deque<int> joined_, lost_;

    /* The last settings record received, and whether it has been taken. Kept
     * rather than queued: only the newest matters, and a client that misses an
     * intermediate change still ends up with the host's current rules. */
    /* One pending parts record per station, newest wins - a peer that changes
     * three things in a second should land the third, not a queue of three. */
    struct PartsNews { int station; uint16_t parts[5]; };
    std::deque<PartsNews> parts_;

    uint8_t settings_[kLsw2ShareBytes] = {0};
    bool settings_pending_ = false;

    /* Byte 6 of the header, per station. 0xFF is "not stated". */
    struct NameNews { int station; char name[24]; };
    std::deque<NameNews> names_;

    uint32_t seed_ = 0;
    bool seed_pending_ = false;

    /* THE HOST'S "the match is starting" signal, host -> clients, flags bit 5.
     * The lobby-active channel the match needs: the DataSharing lockstep only
     * steps once both are IN the match, so a joiner idle in the lobby (state 48)
     * never learns the host left for the arena unless it is told out of band.
     * Carries the chosen map, rule and seconds so the joiner can build the right
     * arena. No block, like the seed. */
    uint8_t start_match_map_ = 0;
    uint8_t start_match_rule_ = 0;
    uint16_t start_match_seconds_ = 0;
    bool start_match_pending_ = false;

    /* Relayed netgame message records, oldest first. */
    struct NetmsgNews { int station; int len; uint8_t bytes[kLsw2ShareBytes]; };
    std::deque<NetmsgNews> netmsgs_;

    uint8_t local_character_ = 0xFF;
    uint8_t peer_character_[kLsw2MaxStations] = {0xFF, 0xFF, 0xFF, 0xFF};
};

#endif /* LSW2_TCP_H */
