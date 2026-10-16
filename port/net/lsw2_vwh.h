/* THE HOST-LEVEL WH: the game's data sharing, supplied by PC networking.
 *
 * The WM contract this reproduces, from the wireless surface the game uses,
 * in short form:
 *
 *   - The game's netgame calls WH_StepDS once a frame with its 124-byte block.
 *     On "no dataset" it waits one vblank and calls again - so returning false
 *     is how a console waits, exactly as on hardware.
 *   - WM's data sharing is a LOCKSTEP pipeline: a dataset is complete when
 *     every sharing member's block for it is in, every console sees the
 *     identical dataset, and each consumed dataset releases one more send (the
 *     first call primes the pipeline). A console that runs late makes the
 *     others wait; a station that stops answering is dropped by WM after a
 *     timeout, not left absent.
 *
 * This class is that contract over a socket, with the pipeline depth L made a
 * parameter so network latency up to L frames costs nothing:
 *
 *   - every console sends its block for slot (receive cursor + L), priming L+1
 *     slots on its first step;
 *   - the HOST commits dataset r once every member that has STARTED sharing has
 *     its block for r in - present = those members, membership = who is
 *     connected - sends COMMIT and consumes it. A member counts from its first
 *     block: a joiner is in the bitmap as soon as it is admitted (the game's
 *     join prompt reads that), and the host does not wait on it until it
 *     actually shares. One that stops sending for kLsw2VwhStallSteps host
 *     steps is kicked, as WM's timeout would;
 *   - the host relays every block as it arrives, so each client holds a
 *     dataset's blocks before its COMMIT (one TCP connection keeps order);
 *   - a CLIENT consumes dataset r once COMMIT(r) is in, and returns false (the
 *     game waits a frame) until then;
 *   - a joiner is QUEUED when it connects and seated (START) when the binding
 *     releases it - the cartridge's lobby takes one joiner at a time - and it
 *     joins the datasets, bitmap included, at its first block; a leaver or a
 *     kicked member is out of the next commit.
 *
 * Membership (who is connected - what WH_GetBitmap reports, and what the game's
 * station count derives from) changes only on join/leave, at the same dataset
 * index on every console. Presence (whose block made it) is per dataset.
 *
 * Guest-free on purpose, like lsw2_lockstep: the ARM9 binding is
 * port/frontend/vwh.cpp, and this half is testable with no ROM. */
#ifndef LSW2_VWH_H
#define LSW2_VWH_H

#include "lsw2_tcp.h"

#include <cstdint>

constexpr int kLsw2VwhBlockBytes = kLsw2ShareBytes;   /* 124: WM dataLength */
constexpr int kLsw2VwhStations = kLsw2MaxStations;    /* 4: aidBitmap 0xF   */
/* Two frames of slack is ~33 ms: everything on one machine or a LAN, and most
 * of a nearby internet connection. The host picks it; clients are told. */
constexpr int kLsw2VwhDefaultDelay = 2;
/* THE GAME CAPS IT, NOT THE NETWORK. The netgame's cross-player checksum
 * (FUN_020AF460) looks each member's reported frame up in an 8-entry history of
 * its own, so two consoles may be at most 7 datasets apart before a perfectly
 * consistent match reads as a mismatch (state 38). Lockstep holds the gap at
 * the pipeline depth, so the depth must stay under 8. */
constexpr int kLsw2VwhMaxDelay = 6;
/* How long the host waits on one member's block before treating it as gone:
 * ten seconds of host steps. Longer than any level load (both consoles load
 * the arena at once and the slower one is waited for), shorter than a player
 * would sit on a frozen screen. */
constexpr uint32_t kLsw2VwhStallSteps = 600;

/* THE PIPELINE DEPTH EACH JOINER IS GIVEN, from its measured round trip.
 *
 * A client sends its block for slot r+L when it consumes dataset r - about one
 * trip after the host committed r - so the block reaches the host a full round
 * trip after the host's step r, and the host needs it by its step r+L. So L
 * datasets must cover the round trip; the game exchanges 30 datasets a second,
 * one per game frame, 33.3 ms each. One more covers jitter, and the result is
 * clamped to [kLsw2VwhDefaultDelay, kLsw2VwhMaxDelay]:
 *
 *     round trip   < 33 ms -> 2      (one machine, a LAN)
 *                 33..66   -> 3
 *                 66..100  -> 4    ...   >= 133 ms -> 6, and past ~160 ms the
 *                                         host waits whatever L is.
 *
 * WHAT A LINK NEEDS, NOT WHAT IT GETS. Every console must run the same depth -
 * the game's own cross-player check depends on it (release_admission has the
 * measurement) - so this is reported, not applied: a link needing more than the
 * session's depth makes frames wait on it. Raising the whole session's depth at
 * a seating is the route to more; it is not built. -1 (not measured) gives the
 * default. */
constexpr int kLsw2VwhDatasetUs = 33333;
int lsw2_vwh_delay_for_rtt_us(int rtt_us);

/* EFFECTS: THE ONE WAY THE FORK MAY CHANGE THE SIMULATION IN A SESSION.
 *
 * The lockstep rule is that nothing may write the game from the between-frames
 * tick while consoles share datasets, because each console's tick lands at a
 * different simulation step. Some writes are the fork's own features - a
 * player's hat, the roster rules, the host's extras - so they need a step that
 * is the same everywhere. The commit is that step: the host stamps effect
 * records on the COMMIT of dataset r, and every console, host included, gets
 * them back from step() with dataset r itself and applies them before the game
 * sees that dataset. Same bytes, same step, every console.
 *
 * A record is [type != 0][length][length bytes]; a commit carries as many whole
 * records as fit in its 124 bytes, in the order they were posted, and the rest
 * wait for the next commit. The session does not interpret them. */
constexpr int kLsw2VwhEffectBytes = kLsw2VwhBlockBytes;
constexpr int kLsw2VwhEffectHeader = 2;

class Lsw2VwhSession {
public:
    /* `transport` is the fork session's, already hosting or joined. A host is
     * AID 0 and live at once; a client is live when the host's START arrives. */
    Lsw2VwhSession(Lsw2TcpTransport* transport, bool is_host,
                   int delay = kLsw2VwhDefaultDelay);

    /* Once a frame, BEFORE step(): the host's control records (client side).
     * Blocks are ingested by step() itself. */
    void service();

    /* HOST: a station connected / left. The fork session owns the transport's
     * join and leave events (it seats names, popups and the kick list from
     * them), so it forwards them here rather than both consuming them. A new
     * station is QUEUED until release_admission(); a departed one is out of the
     * next commit (or simply dropped from the queue). */
    void admit(int station);
    void depart(int station);

    /* HOST: seat the longest-waiting station - send its START. The binding
     * calls this only when the game can take a joiner: the cartridge's lobby
     * takes one at a time, prompt and reload included. The station
     * joins the datasets at its first block. Returns it, or -1 if none. */
    int release_admission();
    /* HOST: connected stations not yet seated. */
    int waiting_admissions() const { return waiting_; }
    /* HOST: the station release_admission() would seat next, or -1. */
    int next_admission() const { return waiting_ > 0 ? queue_[0] : -1; }
    /* HOST: the longest-waiting station has had its round trip measured - a few
     * echoes, or long enough that an echo is not coming - so seating it now
     * gives it the depth its link needs (lsw2_vwh_delay_for_rtt_us). */
    bool admission_measured() const;

    /* THE WH_StepDS CONTRACT. Sends `block` wherever the pipeline has room,
     * then consumes the next dataset if it is complete. On true: `out[aid]`
     * holds each PRESENT member's block, `present` which ones, `members` who is
     * connected. On false the caller reports NO_DATASET and the game waits.
     *
     * `game_call` is false for the caller's own retries of one game call while
     * it holds emulation for a block in flight: only the game's calls count
     * toward the host's stall limit, whose unit is "frames the game waited". */
    bool step(const uint8_t block[kLsw2VwhBlockBytes],
              uint8_t out[kLsw2VwhStations][kLsw2VwhBlockBytes],
              uint16_t* present, uint16_t* members, bool game_call = true,
              uint8_t effects[kLsw2VwhEffectBytes] = nullptr);

    /* HOST: stamp an effect record on the next commit that has room for it.
     * False, posting nothing, for a malformed record or one longer than a whole
     * commit - neither could ever be delivered. */
    bool post_effect(const uint8_t* rec, int n);
    /* HOST: effect bytes posted and not yet stamped on a commit. */
    int pending_effect_bytes() const { return pending_len_; }

    /* Wait up to `wait_us` for the network, then take in whatever arrived. */
    void pump(int wait_us);

    bool hosting() const { return is_host_; }
    /* A client that has not been given its START is not in the session yet. */
    bool live() const { return live_; }
    int aid() const { return aid_; }
    int delay() const { return delay_; }
    /* RAISE THE SESSION'S DEPTH to `d` - called on every console at the one
     * dataset that carries the change (the binding's depth effect), so the
     * depth stays uniform, which the game's own check requires. The
     * next step fills the deeper pipeline with this console's current block,
     * as the first step primes it. Never lowered: a shallower pipeline would
     * skip slots the host then waits on for ever. */
    void raise_delay(int d);
    /* Membership of the last dataset consumed (host: of the next commit). */
    uint16_t members() const { return members_; }
    /* Committed datasets a client holds but has not consumed. More than a
     * couple means this console runs slower than the host and should catch up. */
    int backlog() const;
    /* Host only: drop a member (the game's kick). Takes effect next commit. */
    void kick(int aid);
    uint32_t recv_cursor() const { return recv_; }
    /* Host: the member the next commit is waiting on, or -1, and for how many
     * consecutive steps. What a frozen host screen is waiting for. */
    int waiting_on() const { return waiting_on_; }
    uint32_t stall_steps() const { return stall_steps_; }
    /* Host: the round trip to `station` (the median of its last eight
     * echoes), in microseconds, or -1 before its first echo; and the depth it was
     * (or will be) seated with. */
    int rtt_us(int station) const;
    int delay_for(int station) const;

private:
    static constexpr uint32_t kRing = 256;   /* slots held: ~4 s at 60 fps */
    struct Slot {
        uint32_t slot[kLsw2VwhStations];      /* which slot each block is for */
        bool have[kLsw2VwhStations];
        uint8_t block[kLsw2VwhStations][kLsw2VwhBlockBytes];
    };
    struct Commit {
        bool valid = false;
        uint32_t index = 0;
        uint16_t present = 0, members = 0;
        uint8_t effects[kLsw2VwhEffectBytes] = {};
    };
    /* Host: whole records waiting for a commit, oldest first. Bounded so a
     * binding that posts every frame cannot grow it without limit. */
    static constexpr int kPendingBytes = 4 * kLsw2VwhEffectBytes;
    uint8_t pending_[kPendingBytes] = {};
    int pending_len_ = 0;
    int take_effects(uint8_t out[kLsw2VwhEffectBytes]);

    /* Host: round-trip probes to every connected station (queued or seated),
     * every kPingEveryUs, and the last kRttSamples echoes of each. */
    static constexpr int64_t kPingEveryUs = 250000;
    /* What admission_measured() waits for: this many echoes, or this long. */
    static constexpr int kSeatSamples = 3;
    static constexpr int64_t kSeatWaitUs = 1500000;
    void host_service();
    void ping(int station, int64_t now);
    int64_t last_ping_us_ = 0;
    int64_t queued_at_us_[kLsw2VwhStations] = {0, 0, 0, 0};
    static constexpr int kRttSamples = 8;
    int rtt_samples_[kLsw2VwhStations][kRttSamples] = {};
    int rtt_count_[kLsw2VwhStations] = {0, 0, 0, 0};
    int seated_delay_[kLsw2VwhStations] = {0, 0, 0, 0};

    void ingest();
    void put_block(uint32_t slot, int aid, const uint8_t* block);
    bool have_block(uint32_t slot, int aid) const;
    const uint8_t* block_at(uint32_t slot, int aid) const;
    uint16_t members_at(uint32_t slot) const;

    Lsw2TcpTransport* transport_ = nullptr;
    bool is_host_ = false;
    bool live_ = false;
    bool primed_ = false;
    int aid_ = 0;
    int delay_ = kLsw2VwhDefaultDelay;
    uint32_t recv_ = 0;        /* next dataset to consume */
    uint32_t next_send_ = 0;   /* next slot to fill with this console's block */
    uint16_t members_ = 1;
    /* Host: the slot range in which each seated station is a member - from
     * its first block (kForever until then) to its departure. */
    uint32_t since_[kLsw2VwhStations] = {0, 0, 0, 0};
    uint32_t until_[kLsw2VwhStations] = {0, 0, 0, 0};
    bool member_[kLsw2VwhStations] = {true, false, false, false};
    int queue_[kLsw2VwhStations] = {0, 0, 0, 0};
    int waiting_ = 0;
    int waiting_on_ = -1;
    uint32_t stall_steps_ = 0;
    Slot* ring_ = nullptr;
    Commit commits_[kRing];
    uint32_t newest_commit_ = 0;
    bool any_commit_ = false;
    uint32_t admitted_at_ = 0;   /* client: the slot the host admitted us at */

public:
    ~Lsw2VwhSession();
    Lsw2VwhSession(const Lsw2VwhSession&) = delete;
    Lsw2VwhSession& operator=(const Lsw2VwhSession&) = delete;
};

#endif /* LSW2_VWH_H */
