/* See lsw2_vwh.h. */
#include "lsw2_vwh.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace {
constexpr uint32_t kForever = std::numeric_limits<uint32_t>::max();

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

int lsw2_vwh_delay_for_rtt_us(int rtt_us) {
    if (rtt_us < 0) return kLsw2VwhDefaultDelay;
    const int covered = rtt_us / kLsw2VwhDatasetUs + 2;   /* the trip, plus one */
    return std::clamp(covered, kLsw2VwhDefaultDelay, kLsw2VwhMaxDelay);
}

int Lsw2VwhSession::rtt_us(int station) const {
    if (station < 0 || station >= kLsw2VwhStations) return -1;
    /* THE MEDIAN OF THE LAST FEW ECHOES, not the worst - and not the 75th
     * percentile, which this was. A client answers a ping from its frame loop,
     * and a client that is booting or loading a level runs that loop seconds
     * apart: measured on ONE machine, echoes of 125,
     * 1277 and 2648 ms from consoles a loopback socket away. Three slow echoes
     * of eight made the 75th percentile a load spike, and the session deepened
     * to its maximum for a link with no latency at all. The median needs half
     * the samples to be slow before it moves, which a real link's delay does
     * and a busy frame loop does not. */
    const int n = rtt_count_[station];
    if (n == 0) return -1;
    int sorted[kRttSamples];
    for (int i = 0; i < n; ++i) sorted[i] = rtt_samples_[station][i];
    std::sort(sorted, sorted + n);
    return sorted[n / 2];
}

int Lsw2VwhSession::delay_for(int station) const {
    if (station < 0 || station >= kLsw2VwhStations) return delay_;
    if (station == aid_) return delay_;
    return seated_delay_[station] ? seated_delay_[station]
                                  : lsw2_vwh_delay_for_rtt_us(rtt_us(station));
}

void Lsw2VwhSession::host_service() {
    /* THE ECHOES FIRST, then maybe a new round of probes. The token is the send
     * time in microseconds, truncated - a round trip is far shorter than the
     * 71 minutes it takes to wrap, and the subtraction is done in 32 bits so a
     * wrap between send and echo still measures right. */
    int station = -1;
    uint32_t token = 0;
    const int64_t now = now_us();
    while (transport_->take_vwh_pong(&station, &token)) {
        if (station <= 0 || station >= kLsw2VwhStations) continue;
        const int rtt = int(uint32_t(now) - token);
        if (rtt < 0 || rtt > 10000000) continue;          /* not ours */
        int& n = rtt_count_[station];
        if (n < kRttSamples) {
            rtt_samples_[station][n++] = rtt;
        } else {
            for (int i = 1; i < kRttSamples; ++i)
                rtt_samples_[station][i - 1] = rtt_samples_[station][i];
            rtt_samples_[station][kRttSamples - 1] = rtt;
        }
    }
    if (now - last_ping_us_ < kPingEveryUs) return;
    last_ping_us_ = now;
    for (int s = 1; s < kLsw2VwhStations; ++s) {
        bool connected = member_[s] && until_[s] == kForever;
        for (int i = 0; i < waiting_; ++i)
            if (queue_[i] == s) connected = true;
        if (connected) ping(s, now);
    }
}

void Lsw2VwhSession::ping(int station, int64_t now) {
    transport_->send_vwh(station, kLsw2VwhPing, uint32_t(now), 0, 0);
}

bool Lsw2VwhSession::admission_measured() const {
    if (!is_host_ || waiting_ == 0) return false;
    const int s = queue_[0];
    return rtt_count_[s] >= kSeatSamples ||
           now_us() - queued_at_us_[s] >= kSeatWaitUs;
}

Lsw2VwhSession::Lsw2VwhSession(Lsw2TcpTransport* transport, bool is_host,
                               int delay)
    : transport_(transport), is_host_(is_host) {
    ring_ = new Slot[kRing];
    for (uint32_t i = 0; i < kRing; ++i)
        for (int a = 0; a < kLsw2VwhStations; ++a) {
            ring_[i].slot[a] = 0;
            ring_[i].have[a] = false;
        }
    delay_ = std::clamp(delay, 1, kLsw2VwhMaxDelay);
    if (is_host_) {
        /* The host is AID 0 and a member of every dataset it will ever commit. */
        aid_ = 0;
        live_ = true;
        member_[0] = true;
        since_[0] = 0;
        until_[0] = kForever;
        members_ = 1;
    } else {
        aid_ = -1;
        live_ = false;
        member_[0] = false;
        members_ = 0;
    }
}

Lsw2VwhSession::~Lsw2VwhSession() { delete[] ring_; }

void Lsw2VwhSession::put_block(uint32_t slot, int aid, const uint8_t* block) {
    if (aid < 0 || aid >= kLsw2VwhStations || !block) return;
    Slot& s = ring_[slot % kRing];
    s.slot[aid] = slot;
    s.have[aid] = true;
    std::memcpy(s.block[aid], block, kLsw2VwhBlockBytes);
}

bool Lsw2VwhSession::have_block(uint32_t slot, int aid) const {
    const Slot& s = ring_[slot % kRing];
    return s.have[aid] && s.slot[aid] == slot;
}

const uint8_t* Lsw2VwhSession::block_at(uint32_t slot, int aid) const {
    return have_block(slot, aid) ? ring_[slot % kRing].block[aid] : nullptr;
}

uint16_t Lsw2VwhSession::members_at(uint32_t slot) const {
    uint16_t m = 0;
    for (int a = 0; a < kLsw2VwhStations; ++a)
        if (member_[a] && since_[a] != kForever && slot >= since_[a] &&
            slot < until_[a])
            m = uint16_t(m | (1u << a));
    return m;
}

void Lsw2VwhSession::ingest() {
    if (!transport_) return;
    int frame = 0, from = 0;
    uint8_t block[kLsw2VwhBlockBytes];
    /* Every block record, including this console's own echoed back by the
     * transport - storing that twice is harmless, it is the same bytes. */
    while (transport_->poll(&frame, &from, block)) {
        put_block(uint32_t(frame), from, block);
        /* A MEMBER JOINS THE DATASETS AT ITS FIRST BLOCK - or at the next
         * dataset, if that block is for one already committed without it. One
         * connection delivers a station's blocks in slot order, so the first to
         * arrive is its lowest. From that slot it is in the bitmap and the host
         * waits for it: WM's association, which is when a child first shares,
         * not when its socket connected. */
        if (is_host_ && from > 0 && from < kLsw2VwhStations && member_[from] &&
            until_[from] == kForever && since_[from] == kForever)
            since_[from] = std::max(uint32_t(frame), recv_);
    }
}

void Lsw2VwhSession::service() {
    if (!transport_) return;
    if (is_host_) { host_service(); return; }
    uint8_t sub = 0, a = 0, b = 0;
    uint32_t frame = 0;
    uint8_t payload[kLsw2VwhEffectBytes];
    while (transport_->take_vwh(&sub, &frame, &a, &b, payload)) {
        if (sub == kLsw2VwhPing) {
            transport_->send_vwh_pong(frame);   /* echo at once, token intact */
        } else if (sub == kLsw2VwhStart) {
            aid_ = int(a);
            delay_ = std::clamp(int(b), 1, kLsw2VwhMaxDelay);
            admitted_at_ = frame;
            live_ = true;
            primed_ = false;
            recv_ = next_send_ = frame;
        } else if (sub == kLsw2VwhCommit) {
            Commit& c = commits_[frame % kRing];
            c.valid = true;
            c.index = frame;
            c.present = a;
            c.members = b;
            std::memcpy(c.effects, payload, kLsw2VwhEffectBytes);
            if (!any_commit_ || frame > newest_commit_) newest_commit_ = frame;
            any_commit_ = true;
        }
    }
}

void Lsw2VwhSession::admit(int station) {
    if (!is_host_ || !transport_) return;
    if (station <= 0 || station >= kLsw2VwhStations) return;
    /* QUEUED, NOT SEATED. The cartridge's lobby takes one joiner at a time,
     * so the binding releases them when the game can take one. */
    for (int i = 0; i < waiting_; ++i)
        if (queue_[i] == station) return;
    if (waiting_ < kLsw2VwhStations) {
        queue_[waiting_++] = station;
        /* PROBE AT ONCE, and three more follow at the ping interval: the seat
         * waits for them (admission_measured) so the depth fits the link. */
        rtt_count_[station] = 0;
        queued_at_us_[station] = now_us();
        ping(station, queued_at_us_[station]);
    }
}

int Lsw2VwhSession::release_admission() {
    if (!is_host_ || !transport_ || waiting_ == 0) return -1;
    const int station = queue_[0];
    for (int i = 1; i < waiting_; ++i) queue_[i - 1] = queue_[i];
    --waiting_;
    /* THE HOST'S NEXT UNFILLED SLOT is where the newcomer may begin; it joins
     * the datasets at its first block (ingest), so nobody waits for it before
     * then. */
    const uint32_t at = next_send_;
    member_[station] = true;
    since_[station] = kForever;
    until_[station] = kForever;
    /* THE SESSION'S DEPTH, NOT ITS OWN. A per-client depth fitted
     * to the link was built and measured, and the game's own check refuses it:
     * FUN_020AF460 compares every member's reported frame with this console's
     * history, and starts only once this console's OWN echoed record shows a
     * frame - which covers the other members' first frames only if they arrive
     * no later than its own, i.e. if every console runs the same depth. A joiner
     * seated at 6 beside a host at 2 still reported its pre-match frame 0 after
     * the host had begun checking: state 38 at the arena's first frames. So
     * everyone runs the host's depth; lsw2_vwh_delay_for_rtt_us says what a
     * link would need (the binding reports it), and a link that needs more than
     * the session's depth makes frames wait on it rather than desync. */
    seated_delay_[station] = delay_;
    transport_->send_vwh(station, kLsw2VwhStart, at, uint8_t(station),
                         uint8_t(seated_delay_[station]));
    return station;
}

void Lsw2VwhSession::raise_delay(int d) {
    d = std::clamp(d, 1, kLsw2VwhMaxDelay);
    if (d <= delay_) return;
    delay_ = d;
    for (int a = 0; a < kLsw2VwhStations; ++a)
        if (seated_delay_[a]) seated_delay_[a] = d;
}

void Lsw2VwhSession::depart(int station) {
    if (!is_host_ || station <= 0 || station >= kLsw2VwhStations) return;
    /* A station index is reused by the next joiner: its round trip is not. */
    rtt_count_[station] = 0;
    seated_delay_[station] = 0;
    for (int i = 0; i < waiting_; ++i) {
        if (queue_[i] != station) continue;
        for (int j = i + 1; j < waiting_; ++j) queue_[j - 1] = queue_[j];
        --waiting_;
        return;                       /* never seated, nothing to leave */
    }
    if (!member_[station] || until_[station] != kForever) return;
    if (since_[station] == kForever) {
        member_[station] = false;     /* seated but never shared */
        return;
    }
    until_[station] = recv_;          /* out of the next commit */
}

void Lsw2VwhSession::kick(int aid) {
    if (!is_host_ || !transport_ || aid <= 0 || aid >= kLsw2VwhStations) return;
    /* Out of the next commit NOW - a host waiting on this member's block must
     * not wait another frame for the transport to report the loss. The fork
     * session's later depart() for the same station is then a no-op. */
    depart(aid);
    transport_->disconnect(aid);
}

int Lsw2VwhSession::backlog() const {
    if (is_host_ || !any_commit_ || newest_commit_ + 1 < recv_) return 0;
    return int(newest_commit_ + 1 - recv_);
}

void Lsw2VwhSession::pump(int wait_us) {
    if (!transport_) return;
    if (wait_us > 0) transport_->wait_readable(wait_us);
    transport_->service();
    service();
}

bool Lsw2VwhSession::post_effect(const uint8_t* rec, int n) {
    if (!is_host_ || !rec || n < kLsw2VwhEffectHeader || n > kLsw2VwhEffectBytes)
        return false;
    if (rec[0] == 0 || int(rec[1]) + kLsw2VwhEffectHeader != n) return false;
    if (pending_len_ + n > kPendingBytes) return false;
    std::memcpy(pending_ + pending_len_, rec, size_t(n));
    pending_len_ += n;
    return true;
}

int Lsw2VwhSession::take_effects(uint8_t out[kLsw2VwhEffectBytes]) {
    /* WHOLE RECORDS, OLDEST FIRST, as many as fit. A record that does not fit
     * waits - and so does everything posted after it, so effects are applied
     * in the order they were posted. */
    std::memset(out, 0, kLsw2VwhEffectBytes);
    int used = 0;
    while (used < pending_len_) {
        const int n = int(pending_[used + 1]) + kLsw2VwhEffectHeader;
        if (used + n > kLsw2VwhEffectBytes) break;
        used += n;
    }
    std::memcpy(out, pending_, size_t(used));
    std::memmove(pending_, pending_ + used, size_t(pending_len_ - used));
    pending_len_ -= used;
    return used;
}

bool Lsw2VwhSession::step(const uint8_t block[kLsw2VwhBlockBytes],
                          uint8_t out[kLsw2VwhStations][kLsw2VwhBlockBytes],
                          uint16_t* present, uint16_t* members, bool game_call,
                          uint8_t effects[kLsw2VwhEffectBytes]) {
    if (!transport_ || !live_ || !block) return false;
    ingest();
    service();

    if (!primed_) {
        if (!is_host_) {
            /* START AT THE HOST'S PRESENT, not where we were admitted. The game
             * reaches data sharing some frames after the socket is admitted, and
             * a console that began at the admission slot would start that many
             * datasets behind and stay there. It is simply absent from the
             * datasets in between - it was not running yet. */
            uint32_t start = admitted_at_;
            if (any_commit_ && newest_commit_ + 1 > start) start = newest_commit_ + 1;
            recv_ = next_send_ = start;
        }
        primed_ = true;
    }

    /* PRIME AND KEEP THE PIPELINE FULL: this console's block for every slot up
     * to recv + L. On the first call that is L+1 slots of the same block - WM's
     * first call primes the same way - and afterwards one slot per consumed
     * dataset. */
    while (next_send_ <= recv_ + uint32_t(delay_)) {
        put_block(next_send_, aid_, block);
        transport_->send(int(next_send_), aid_, block);
        ++next_send_;
    }
    /* NO SENDING AHEAD OF THE HOST'S FRONTIER. An earlier version aimed a
     * lagging client's block at (newest commit + L) because the host never
     * waited and a late block was simply absent. Under lockstep a client can
     * never trail the host's commits by more than L, and a skipped slot would be
     * a hole the host waits on forever. */

    uint16_t pres = 0, mem = 0;
    if (is_host_) {
        /* THE HOST WAITS FOR EVERY MEMBER THAT SHARES - WM's lockstep.
         *
         * Measured: with a host that never waited, the
         * joiner finished loading the arena later than the host, its blocks
         * missed dataset after dataset, and the first check of the match -
         * FUN_020AF460, each member's checksum looked up in an 8-frame history
         * by that member's frame number - compared against frames the history
         * had already overwritten. State 38 in the first second of every match,
         * with nothing actually out of step. On hardware the parent waits, the
         * gap stays at the pipeline depth, and the history always holds it. */
        mem = members_at(recv_);
        int wait_for = -1;
        for (int a = 0; a < kLsw2VwhStations && wait_for < 0; ++a)
            if (((mem >> a) & 1u) && recv_ >= since_[a] &&
                !have_block(recv_, a))
                wait_for = a;
        if (wait_for >= 0) {
            if (waiting_on_ != wait_for) stall_steps_ = 0;
            waiting_on_ = wait_for;
            if (game_call && ++stall_steps_ >= kLsw2VwhStallSteps && wait_for > 0)
                kick(wait_for);
            return false;
        }
        waiting_on_ = -1;
        stall_steps_ = 0;
        for (int a = 0; a < kLsw2VwhStations; ++a)
            if (((mem >> a) & 1u) && have_block(recv_, a))
                pres = uint16_t(pres | (1u << a));
        /* WM PARITY: a parent with no children steps "successfully" with
         * nobody in the dataset (WM_StepDataSharing's NO_CHILD, which WH_StepDS
         * treats as success without touching the shared data). */
        if (mem == 1u) pres = 0;
        uint8_t stamped[kLsw2VwhEffectBytes];
        const int n = take_effects(stamped);
        transport_->send_vwh(-1, kLsw2VwhCommit, recv_, uint8_t(pres),
                             uint8_t(mem), n > 0 ? stamped : nullptr, n);
        if (effects) std::memcpy(effects, stamped, kLsw2VwhEffectBytes);
    } else {
        const Commit& c = commits_[recv_ % kRing];
        if (!c.valid || c.index != recv_) return false;
        pres = c.present;
        mem = c.members;
        /* The host relays every block before the commit that names it, on the
         * one connection, so a named block that is not here yet is only a frame
         * of ordering - wait it out rather than consume a hole. */
        for (int a = 0; a < kLsw2VwhStations; ++a)
            if (((pres >> a) & 1u) && !have_block(recv_, a)) return false;
        if (effects) std::memcpy(effects, c.effects, kLsw2VwhEffectBytes);
    }

    for (int a = 0; a < kLsw2VwhStations; ++a) {
        const uint8_t* src = ((pres >> a) & 1u) ? block_at(recv_, a) : nullptr;
        if (src) std::memcpy(out[a], src, kLsw2VwhBlockBytes);
        else std::memset(out[a], 0, kLsw2VwhBlockBytes);
    }
    if (present) *present = pres;
    if (members) *members = mem;
    members_ = mem;
    ++recv_;
    return true;
}
