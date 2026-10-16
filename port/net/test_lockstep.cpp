/* Prove the lockstep session with no ROM, no window and no sockets.
 *
 * Four stations run in one process over an in-memory transport that can be told
 * to delay or drop a station's traffic. That is the whole reason the transport
 * is an interface: the properties worth testing - frame alignment, stalling
 * rather than desyncing, surviving a disconnect - are properties of the
 * algorithm, and testing them over real sockets would only add flakiness.
 *
 *   g++ -std=c++20 -I. test_lockstep.cpp lsw2_lockstep.cpp -o test_lockstep
 */

#include "lsw2_lockstep.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/* A shared bus every station writes into and reads from. `held[i]` withholds
 * station i's traffic, which is how a lagging or vanished peer is simulated. */
struct Bus {
    struct Msg { int frame, from; uint8_t block[kLsw2ShareBytes]; };
    std::deque<Msg> pending[kLsw2MaxStations];   /* per RECIPIENT */
    bool held[kLsw2MaxStations] = {false,false,false,false};

    void publish(int frame, int from, const uint8_t* block) {
        if (held[from]) return;
        for (int to = 0; to < kLsw2MaxStations; ++to) {
            if (to == from) continue;
            Msg m{frame, from, {}};
            std::memcpy(m.block, block, kLsw2ShareBytes);
            pending[to].push_back(m);
        }
    }
};

class BusTransport : public Lsw2NetTransport {
public:
    BusTransport(Bus* bus, int me) : bus_(bus), me_(me) {}
    void send(int frame, int from, const uint8_t* block) override {
        bus_->publish(frame, from, block);
        /* A station always has its own block: on the DS it is in the shared
         * buffer alongside everyone else's. */
        Bus::Msg m{frame, from, {}};
        std::memcpy(m.block, block, kLsw2ShareBytes);
        self_.push_back(m);
    }
    bool poll(int* frame, int* from, uint8_t* block) override {
        if (!self_.empty()) {
            const Bus::Msg m = self_.front(); self_.pop_front();
            *frame = m.frame; *from = m.from;
            std::memcpy(block, m.block, kLsw2ShareBytes);
            return true;
        }
        auto& q = bus_->pending[me_];
        if (q.empty()) return false;
        const Bus::Msg m = q.front(); q.pop_front();
        *frame = m.frame; *from = m.from;
        std::memcpy(block, m.block, kLsw2ShareBytes);
        return true;
    }
    void disconnect(int) override {}
private:
    Bus* bus_;
    int me_;
    std::deque<Bus::Msg> self_;
};

/* A block whose first bytes name the station and frame, so a mix-up between
 * stations or frames is visible rather than silent. */
void fill(uint8_t* b, int station, int frame) {
    std::memset(b, 0, kLsw2ShareBytes);
    b[0] = uint8_t(station);
    b[1] = uint8_t(frame & 0xFF);
    b[2] = uint8_t((frame >> 8) & 0xFF);
    b[kLsw2ShareBytes - 1] = 0xA5;   /* catches a short copy */
}

}  // namespace

int main() {
    std::printf("sizes\n");
    check("124-byte blocks, from WM_StartDataSharing", kLsw2ShareBytes == 124);
    check("four stations, from aidBitmap 0xF", kLsw2MaxStations == 4);

    Bus bus;
    std::vector<BusTransport> tr;
    tr.reserve(kLsw2MaxStations);
    for (int i = 0; i < kLsw2MaxStations; ++i) tr.emplace_back(&bus, i);

    std::vector<Lsw2LockstepSession> s;
    s.reserve(kLsw2MaxStations);
    for (int i = 0; i < kLsw2MaxStations; ++i)
        s.emplace_back(i == 0 ? Lsw2NetRole::Host : Lsw2NetRole::Client, i, &tr[i]);
    for (int i = 0; i < kLsw2MaxStations; ++i)
        for (int j = 0; j < kLsw2MaxStations; ++j)
            s[i].seat(j, j == 0 ? "Maclunkey" : "PLAYER", i == j);

    std::printf("seating\n");
    check("a fifth station is refused", !s[0].seat(4, "x", false));
    check("a duplicate seat is refused", !s[0].seat(1, "x", false));

    std::printf("frame alignment\n");
    uint8_t out[kLsw2MaxStations][kLsw2ShareBytes];
    uint16_t bitmap = 0;
    bool all_ok = true, content_ok = true, opening_free = true;
    const int delay = s[0].delay_frames();

    for (int f = 0; f < 40; ++f) {
        for (int i = 0; i < kLsw2MaxStations; ++i) {
            uint8_t blk[kLsw2ShareBytes];
            fill(blk, i, f);
            const bool ok = s[i].step(blk, out, &bitmap);
            if (!ok) all_ok = false;
            if (f < delay && !ok) opening_free = false;
            if (ok && f >= delay) {
                const int consuming = f - delay;
                for (int j = 0; j < kLsw2MaxStations; ++j) {
                    if (out[j][0] != j) content_ok = false;
                    if (out[j][1] != uint8_t(consuming & 0xFF)) content_ok = false;
                    if (out[j][kLsw2ShareBytes - 1] != 0xA5) content_ok = false;
                }
                if (bitmap != 0xF) content_ok = false;
            }
        }
    }
    check("no station ever stalled", all_ok);
    check("the opening frames run free", opening_free);
    check("every station got every other station's block, for the right frame",
          content_ok);
    check("no stalls recorded", s[0].stalls() == 0);

    std::printf("a station that stops sending stalls the others, it does not desync\n");
    bus.held[2] = true;
    bool stalled = false;
    /* STEP EVERY STATION THAT IS STILL ALIVE, not just station 0. The first
     * version of this test stepped only 0, which then stalled waiting for 1 and
     * 3 - blocks nobody was producing, because those sessions were not being
     * driven. That is a fault in the test rather than the session, and it is
     * exactly the deadlock the "send first" rule exists to avoid in real use. */
    for (int f = 0; f < delay + 3; ++f) {
        for (int i : {0, 1, 3}) {
            uint8_t blk[kLsw2ShareBytes];
            fill(blk, i, s[i].frame());
            if (!s[i].step(blk, out, &bitmap) && i == 0) stalled = true;
        }
    }
    check("the session refuses to advance without station 2", stalled);
    check("and counts it", s[0].stalls() > 0);

    std::printf("dropping that station lets the rest continue\n");
    for (int i : {0, 1, 3}) s[i].drop(2);
    bool resumed = false;
    for (int f = 0; f < delay + 6; ++f) {
        for (int i : {0, 1, 3}) {
            uint8_t blk[kLsw2ShareBytes];
            fill(blk, i, s[i].frame());
            const bool ok = s[i].step(blk, out, &bitmap);
            if (ok && i == 0) resumed = true;
        }
    }
    check("the session advances again", resumed);
    check("station 2 is gone from the bitmap", (bitmap & (1u << 2)) == 0);
    check("the others are still present", (bitmap & 0x1) != 0);

    std::printf("delay is clamped to sane bounds\n");
    s[0].set_delay_frames(0);
    check("below the floor clamps up", s[0].delay_frames() == kLsw2MinDelayFrames);
    s[0].set_delay_frames(9999);
    check("above the ceiling clamps down", s[0].delay_frames() == kLsw2MaxDelayFrames);

    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
