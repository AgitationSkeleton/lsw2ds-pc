/* See lsw2_lockstep.h. */

#include "lsw2_lockstep.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

Lsw2LockstepSession::Lsw2LockstepSession(Lsw2NetRole role, int local_station,
                                         Lsw2NetTransport* transport)
    : role_(role), local_(local_station), transport_(transport) {
    if (local_ < 0 || local_ >= kLsw2MaxStations) local_ = 0;
}

bool Lsw2LockstepSession::seat(int station, const char* name, bool local) {
    /* A FIFTH STATION IS REFUSED. Four is the arena's own width - the handler-0
     * constructor counts exactly four slots and the ROM carries four slot
     * palettes - so this is the game's limit rather than a policy. */
    if (station < 0 || station >= kLsw2MaxStations) return false;
    if (stations_[station].present) return false;

    stations_[station].present = true;
    std::snprintf(stations_[station].name, sizeof stations_[station].name,
                  "%s", name && *name ? name : "PLAYER");
    if (local) local_ = station;
    state_ = Lsw2NetState::Ready;
    return true;
}

void Lsw2LockstepSession::drop(int station) {
    if (station < 0 || station >= kLsw2MaxStations) return;
    if (!stations_[station].present) return;

    /* GRACEFULLY, which is a requirement in its own right: the station simply stops
     * being present and its queued blocks go with it. The frame after this one
     * completes without it rather than stalling forever waiting for a peer that
     * has gone.
     *
     * The game already handles a station vanishing - WM_STATECODE_DISCONNECTED
     * and "StartParent - child (aid %x) disconnected" are in the image - so the
     * disconnect popup is presentation over an event the guest
     * understands. */
    stations_[station] = Lsw2Station{};
    queue_[station].clear();
    if (transport_) transport_->disconnect(station);
    if (station == local_) state_ = Lsw2NetState::Lost;
}

void Lsw2LockstepSession::set_delay_frames(int frames) {
    delay_ = std::clamp(frames, kLsw2MinDelayFrames, kLsw2MaxDelayFrames);
}

void Lsw2LockstepSession::ingest() {
    if (!transport_) return;
    int frame = 0, from = 0;
    uint8_t block[kLsw2ShareBytes];
    while (transport_->poll(&frame, &from, block)) {
        if (from < 0 || from >= kLsw2MaxStations) continue;
        if (!stations_[from].present) continue;

        /* DROP WHAT IS ALREADY PAST. A block for a frame we have consumed is
         * not useful and keeping it would let the queue grow without bound on a
         * peer that reconnects mid-session. */
        const int consuming = frame_ - delay_;
        if (frame < consuming) continue;

        /* KEEP WHAT ARRIVES EARLY. A peer one frame ahead is normal - the
         * delay buffer exists precisely so that happens - and discarding it
         * would turn ordinary jitter into a stall. */
        Pending p;
        p.frame = frame;
        std::memcpy(p.block, block, kLsw2ShareBytes);
        queue_[from].push_back(p);
    }
}

bool Lsw2LockstepSession::step(const uint8_t* local_block,
                               uint8_t out[kLsw2MaxStations][kLsw2ShareBytes],
                               uint16_t* bitmap) {
    /* SEND FIRST, ALWAYS. Sending before checking whether we can advance means
     * a station that is stalled waiting for a peer is still feeding that peer -
     * otherwise two stations each waiting for the other would deadlock, which
     * is the classic way a lockstep session dies. */
    if (transport_ && local_block)
        transport_->send(frame_, local_, local_block);

    ingest();

    const int consuming = frame_ - delay_;
    if (consuming < 0) {
        /* THE OPENING FRAMES ARE FREE. Before the delay has elapsed there is
         * nothing to consume yet, and the guest should run with empty input
         * rather than stall - which is exactly what the DS does for the first
         * frames after DataSharing starts. */
        std::memset(out, 0, size_t(kLsw2MaxStations) * kLsw2ShareBytes);
        if (bitmap) *bitmap = 0;
        ++frame_;
        return true;
    }

    /* Every seated station must have the frame being consumed. Missing one is
     * not an error to paper over: advancing without it is the desync the game's
     * own checksum is there to catch. */
    for (int i = 0; i < kLsw2MaxStations; ++i) {
        if (!stations_[i].present) continue;
        bool have = false;
        for (const Pending& p : queue_[i])
            if (p.frame == consuming) { have = true; break; }
        if (!have) {
            ++stalls_;
            return false;
        }
    }

    uint16_t mask = 0;
    std::memset(out, 0, size_t(kLsw2MaxStations) * kLsw2ShareBytes);
    for (int i = 0; i < kLsw2MaxStations; ++i) {
        if (!stations_[i].present) continue;
        for (const Pending& p : queue_[i]) {
            if (p.frame != consuming) continue;
            std::memcpy(out[i], p.block, kLsw2ShareBytes);
            mask |= uint16_t(1u << i);
            break;
        }
        /* Retire everything up to and including the frame just consumed. */
        auto& q = queue_[i];
        q.erase(std::remove_if(q.begin(), q.end(),
                               [consuming](const Pending& p) {
                                   return p.frame <= consuming;
                               }),
                q.end());
    }

    /* The bitmap is by station index, which is the AID the guest uses - the
     * same width and meaning as the u16 the game keeps at block+0x66. */
    if (bitmap) *bitmap = mask;
    ++frame_;
    return true;
}
