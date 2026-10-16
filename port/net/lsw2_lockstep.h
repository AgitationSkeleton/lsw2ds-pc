/* The lockstep session: what WM DataSharing is, over a socket.
 *
 * WHY THIS SHAPE. LSW2's netplay is this one call, and it decides this file:
 *
 *     WM_StartDataSharing(info, port=13, aidBitmap=0xF, dataLength=124, ...)
 *
 * Every station contributes a fixed 124-byte block each frame and receives
 * every station's block before the frame advances. That is deterministic
 * lockstep, already, in the shipped game - so the port does not get to choose a
 * netcode model. It has to provide this one.
 *
 * WHAT THIS FILE IS AND IS NOT. It is the transport: frame-aligned exchange of
 * fixed-size blocks between up to four peers, with a delay buffer, join and
 * loss handling. It is NOT the WH/WM HLE - nothing here knows about the guest,
 * the ARM9, or `0x0210B520`. That binding is a separate layer, and keeping them
 * apart is what makes this one testable with no ROM and no window.
 *
 * THE NUMBERS ARE THE GAME'S, not choices:
 *   - 124 bytes, from the dataLength argument at both call sites
 *   - four stations, from aidBitmap 0xF and the arena's four slots
 *   - one block per station per frame, from DataSharing's own contract
 */

#ifndef LSW2_LOCKSTEP_H
#define LSW2_LOCKSTEP_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* From WM_StartDataSharing's arguments. Not tunable: a different size is a
 * different protocol and the guest would reject it. */
constexpr int kLsw2ShareBytes = 124;
constexpr int kLsw2MaxStations = 4;

/* HOW FAR AHEAD THE LOCAL STATION RUNS.
 *
 * On the DS this is zero - the radio is fast enough that a station can send and
 * receive within one frame. Over the internet it cannot, so the local station
 * sends frame N and consumes frame N-delay, which buys `delay` frames of
 * network time without ever stalling.
 *
 * Two frames is 33ms and is a floor rather than a recommendation; the session
 * raises it when the measured round trip needs more. Kept explicit because a
 * hidden delay is the thing players feel and cannot explain. */
constexpr int kLsw2MinDelayFrames = 2;
constexpr int kLsw2MaxDelayFrames = 12;

enum class Lsw2NetRole { Host, Client };

/* What a station is doing. Mirrors the parts of WH_SYSSTATE the transport can
 * meaningfully be in; the full enum belongs to the HLE layer. */
enum class Lsw2NetState { Idle, Connecting, Ready, Lost };

struct Lsw2Station {
    bool present = false;
    /* THERE IS NO PER-STATION `local` FLAG, and there was.
     *
     * `seat()` set one and nothing ever read it, while `local_` - which IS read,
     * through `local_station()` - held the same fact. Two copies of one truth
     * that could disagree: seating a second local station moved `local_` and
     * left the first station's flag set, so both would have claimed to be this
     * machine. Invisible only because no consumer existed to be misled.
     *
     * Found by sweeping for struct fields written but never read, which is the
     * mirror of the sweep that found `Lsw2SessionState::InMatch` read but never
     * produced. */
    char name[24] = {0};
    /* Round trip in milliseconds, measured. Shown in the lobby list, which is
     * why the master server does not supply it - see that service's README. */
    int ping_ms = 0;
};

/* The transport a session runs over. Abstracted so the tests can run four
 * stations in one process with no sockets at all - which is what makes this
 * layer provable before any of the guest binding exists. */
class Lsw2NetTransport {
public:
    virtual ~Lsw2NetTransport() = default;
    /* Send one station's block for one frame. */
    virtual void send(int frame, int from_station, const uint8_t* block) = 0;
    /* Collect whatever has arrived. Returns false when nothing new. */
    virtual bool poll(int* frame, int* from_station, uint8_t* block) = 0;
    virtual void disconnect(int station) = 0;
};

class Lsw2LockstepSession {
public:
    Lsw2LockstepSession(Lsw2NetRole role, int local_station,
                        Lsw2NetTransport* transport);

    /* Seat a station. Refuses a fifth: four is the arena's own width, not a
     * number chosen here. */
    bool seat(int station, const char* name, bool local);
    void drop(int station);

    int delay_frames() const { return delay_; }
    void set_delay_frames(int frames);

    /* THE PER-FRAME CALL, and the whole point of the class.
     *
     * Hands over this station's block for the current frame and asks for every
     * station's block for the frame being consumed. Returns false when a
     * station's block for that frame has not arrived - the caller must then NOT
     * advance the guest, because advancing without it is precisely the desync
     * the game's own checksum would catch.
     *
     * `out` receives kLsw2MaxStations blocks; absent stations are zeroed and
     * their bit is clear in `bitmap`. The bitmap is by station index, which is
     * the AID the game uses: the connected-station bitmap the game builds is
     * indexed by AID. */
    bool step(const uint8_t* local_block,
              uint8_t out[kLsw2MaxStations][kLsw2ShareBytes],
              uint16_t* bitmap);

    /* Frames the session could not complete because a block was missing. The
     * number a player experiences as stutter, and worth showing rather than
     * hiding. */
    uint64_t stalls() const { return stalls_; }
    int frame() const { return frame_; }
    Lsw2NetState state() const { return state_; }
    const Lsw2Station& station(int i) const { return stations_[i]; }

private:
    struct Pending {
        int frame = -1;
        uint8_t block[kLsw2ShareBytes] = {0};
    };

    void ingest();

    Lsw2NetRole role_;
    int local_ = 0;
    Lsw2NetTransport* transport_ = nullptr;
    Lsw2Station stations_[kLsw2MaxStations];
    /* A small ring per station. Sized to the maximum delay plus slack, because
     * a block that arrives early must be kept, not dropped - a peer running a
     * frame ahead is normal, not an error. */
    std::vector<Pending> queue_[kLsw2MaxStations];
    int delay_ = kLsw2MinDelayFrames;
    int frame_ = 0;
    uint64_t stalls_ = 0;
    Lsw2NetState state_ = Lsw2NetState::Idle;
};

#endif /* LSW2_LOCKSTEP_H */
