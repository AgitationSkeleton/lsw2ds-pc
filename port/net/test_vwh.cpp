/* The host-level WH's data sharing over real loopback sockets (lsw2_vwh.h).
 *
 * What must hold, and what this checks:
 *   - every console that consumes dataset N consumes the IDENTICAL dataset N -
 *     same membership, same presence, same bytes - or the deterministic
 *     simulations diverge (the game's state 38);
 *   - a lone host steps every frame (WM's NO_CHILD);
 *   - LOCKSTEP: once a member shares, it is present in every dataset; a member
 *     that stalls (a level load) makes the host wait rather than go absent, and
 *     no console ever gets more than the pipeline depth ahead of another - the
 *     game's checksum history is only 8 frames deep (FUN_020AF460);
 *   - a joiner admitted mid-stream is a member at the same index everywhere and
 *     is not waited for until it shares;
 *   - a member that stops sending is kicked at the stall limit, and a departed
 *     member leaves the membership at one index, on every console.
 *
 *   g++ -std=c++20 -I. test_vwh.cpp lsw2_vwh.cpp lsw2_tcp.cpp -o test_vwh -lws2_32
 */
#include "lsw2_vwh.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

namespace {
/* The binding's depth effect type (frontend/vwh.h kLsw2EffectDepth), which
 * this test plays the binding's part for. */
constexpr uint8_t kDepthEffect = 2;
int g_failures = 0;
void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Dataset {
    uint16_t present = 0, members = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> effects;     /* the host's stamp on this commit */
    bool operator==(const Dataset& o) const {
        return present == o.present && members == o.members && bytes == o.bytes &&
               effects == o.effects;
    }
};

/* One console: its transport, its session, and every dataset it consumed. */
struct Console {
    Lsw2TcpTransport t;
    Lsw2VwhSession* s = nullptr;
    std::map<uint32_t, Dataset> seen;
    int steps = 0, stalls = 0;
    int frame = 0;
    void step() {
        uint8_t block[kLsw2VwhBlockBytes];
        std::memset(block, 0, sizeof block);
        block[0] = uint8_t(s->aid());
        block[1] = uint8_t(frame & 0xFF);
        block[2] = uint8_t(frame >> 8);
        block[kLsw2VwhBlockBytes - 1] = 0xA5;
        uint8_t out[kLsw2VwhStations][kLsw2VwhBlockBytes];
        uint16_t present = 0, members = 0;
        uint8_t effects[kLsw2VwhEffectBytes];
        const uint32_t index = s->recv_cursor();
        if (s->step(block, out, &present, &members, true, effects)) {
            Dataset d;
            d.present = present;
            d.members = members;
            d.bytes.assign(&out[0][0], &out[0][0] + sizeof out);
            d.effects.assign(effects, effects + kLsw2VwhEffectBytes);
            seen[index] = d;
            /* What the binding does with a depth effect (frontend/vwh.cpp). */
            for (int off = 0; off + 2 <= kLsw2VwhEffectBytes && effects[off] != 0;
                 off += 2 + effects[off + 1])
                if (effects[off] == kDepthEffect && effects[off + 1] >= 1 &&
                    off + 2 < kLsw2VwhEffectBytes)
                    s->raise_delay(effects[off + 2]);
            ++steps;
            ++frame;         /* the game advances only on a dataset */
        } else {
            ++stalls;
        }
    }
};

void pump(Console* c[], int n, Lsw2VwhSession* host_session) {
    for (int i = 0; i < n; ++i) {
        c[i]->t.service();
        if (i == 0) {
            for (int j = c[0]->t.take_joined(); j >= 0; j = c[0]->t.take_joined())
                host_session->admit(j);
            /* The game's binding releases one at a time when the lobby can
             * take a joiner; the test's lobby always can. */
            while (host_session->release_admission() >= 0) {}
            for (int l = c[0]->t.take_lost(); l >= 0; l = c[0]->t.take_lost())
                host_session->depart(l);
        }
        c[i]->s->service();
    }
}

bool agree(const Console& a, const Console& b, int* compared) {
    int n = 0;
    for (const auto& [index, d] : a.seen) {
        auto it = b.seen.find(index);
        if (it == b.seen.end()) continue;
        ++n;
        if (!(it->second == d)) {
            std::printf("    mismatch at dataset %u: present %04X/%04X members %04X/%04X\n",
                        unsigned(index), d.present, it->second.present, d.members,
                        it->second.members);
            return false;
        }
    }
    if (compared) *compared = n;
    return true;
}
}  // namespace

int main() {
    Console host, a, b;
    const uint16_t port = host.t.host(0);
    host.s = new Lsw2VwhSession(&host.t, true, 2);
    std::printf("host on port %u, delay %d\n", port, host.s->delay());

    /* Host alone for a while: it must step every frame (never waits) with
     * nobody present - WM's NO_CHILD. */
    Console* all[3] = {&host, &a, &b};
    for (int f = 0; f < 30; ++f) { pump(all, 1, host.s); host.step(); }
    check("a lone host steps every frame", host.steps == 30 && host.stalls == 0);
    check("a lone host's datasets have nobody present (WM NO_CHILD parity)",
          host.seen.rbegin()->second.present == 0 &&
              host.seen.rbegin()->second.members == 1);

    /* First joiner. */
    check("joiner A connects", a.t.join("127.0.0.1", port));
    a.s = new Lsw2VwhSession(&a.t, false);
    for (int f = 0; f < 120; ++f) {
        pump(all, 2, host.s);
        host.step();
        if (a.s->live()) a.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check("joiner A is live with AID 1", a.s->live() && a.s->aid() == 1);
    check("joiner A consumed datasets", a.steps > 60);
    int compared = 0;
    check("host and A agree on every shared dataset", agree(host, a, &compared));
    std::printf("    (%d datasets compared)\n", compared);
    {
        const Dataset& last = host.seen.rbegin()->second;
        check("both are members and present in the host's latest dataset",
              last.members == 0x3 && last.present == 0x3);
    }

    /* Second joiner, mid-stream. */
    check("joiner B connects", b.t.join("127.0.0.1", port));
    b.s = new Lsw2VwhSession(&b.t, false);
    for (int f = 0; f < 150; ++f) {
        pump(all, 3, host.s);
        host.step();
        if (a.s->live()) a.step();
        if (b.s->live()) b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check("joiner B is live with AID 2", b.s->live() && b.s->aid() == 2);
    check("host and A agree", agree(host, a, &compared));
    check("host and B agree", agree(host, b, &compared));
    check("A and B agree", agree(a, b, &compared));
    {
        const Dataset& last = host.seen.rbegin()->second;
        check("all three members and present at the end",
              last.members == 0x7 && last.present == 0x7);
    }
    /* EFFECTS: the host stamps records on a commit and every console
     * gets the same bytes back with the same dataset - agree() compares them. */
    {
        const uint8_t hat[] = {0x10, 3, 2, 0x44, 0x55};        /* type, len, 3 bytes */
        const uint8_t roster[] = {0x11, 1, 7};
        uint8_t big[kLsw2VwhEffectBytes] = {0x12, uint8_t(kLsw2VwhEffectBytes - 2)};
        check("a malformed effect is refused", !host.s->post_effect(hat, 4));
        check("an effect with type 0 is refused",
              !host.s->post_effect((const uint8_t[]){0, 0}, 2));
        check("the host posts effects", host.s->post_effect(hat, sizeof hat) &&
                                            host.s->post_effect(big, sizeof big) &&
                                            host.s->post_effect(roster, sizeof roster));
        check("a client cannot post one itself", !a.s->post_effect(hat, sizeof hat));
        const uint8_t late[] = {0x13, 2, 9, 9};
        check("one posted later still goes out", host.s->post_effect(late, sizeof late));
        for (int f = 0; f < 60; ++f) {
            pump(all, 3, host.s);
            host.step();
            a.step();
            b.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        /* Where each record landed, on the host. */
        uint32_t at_hat = 0, at_big = 0, at_roster = 0, at_ask = 0;
        for (const auto& [index, d] : host.seen) {
            if (d.effects.size() < 2) continue;
            for (int off = 0; off + 1 < int(d.effects.size()) && d.effects[off];
                 off += 2 + d.effects[off + 1]) {
                if (d.effects[off] == 0x10) at_hat = index;
                if (d.effects[off] == 0x12) at_big = index;
                if (d.effects[off] == 0x11) at_roster = index;
                if (d.effects[off] == 0x13) at_ask = index;
            }
        }
        check("every record was stamped", at_hat && at_big && at_roster && at_ask);
        check("records that do not fit wait for a later commit, in order",
              at_hat < at_big && at_big < at_roster && at_roster <= at_ask);
        check("host and A agree on every commit's effects", agree(host, a, &compared));
        check("A and B agree on every commit's effects", agree(a, b, &compared));
        check("nothing is left pending", host.s->pending_effect_bytes() == 0);
    }

    /* THE DEPTH A JOINER IS GIVEN: the round trip in 33.3 ms
     * datasets, plus one, clamped - and measured, over loopback, as the
     * default. */
    check("an unmeasured round trip gets the default depth",
          lsw2_vwh_delay_for_rtt_us(-1) == kLsw2VwhDefaultDelay);
    check("a LAN round trip (20 ms) gets 2",
          lsw2_vwh_delay_for_rtt_us(20000) == 2);
    check("40 ms gets 3, 70 ms gets 4, 100 ms gets 5",
          lsw2_vwh_delay_for_rtt_us(40000) == 3 &&
              lsw2_vwh_delay_for_rtt_us(70000) == 4 &&
              lsw2_vwh_delay_for_rtt_us(100000) == 5);
    check("the depth never passes the checksum history's limit",
          lsw2_vwh_delay_for_rtt_us(500000) == kLsw2VwhMaxDelay);
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((host.s->rtt_us(1) < 0 || host.s->rtt_us(2) < 0) &&
               std::chrono::steady_clock::now() < until) {
            pump(all, 3, host.s);
            host.step();
            a.step();
            b.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::printf("    loopback round trips: A %d us, B %d us\n",
                    host.s->rtt_us(1), host.s->rtt_us(2));
        check("the host measures each joiner's round trip",
              host.s->rtt_us(1) >= 0 && host.s->rtt_us(2) >= 0);
        check("a joiner on this machine is seated at the default depth",
              a.s->delay() == kLsw2VwhDefaultDelay &&
                  host.s->delay_for(1) == kLsw2VwhDefaultDelay);
    }

    /* A dataset's bytes carry each sender's AID in byte 0 of its slot. */
    {
        const Dataset& last = a.seen.rbegin()->second;
        bool ok = true;
        for (int aid = 0; aid < 3; ++aid)
            ok = ok && last.bytes[size_t(aid) * kLsw2VwhBlockBytes] == uint8_t(aid) &&
                 last.bytes[size_t(aid) * kLsw2VwhBlockBytes + kLsw2VwhBlockBytes - 1] == 0xA5;
        check("each member's block sits in its own AID slot", ok);
    }

    /* B STALLS (a level load): it stops stepping for 40 frames. Lockstep means
     * the host WAITS - it does not commit B absent - and A waits with it. */
    const int host_stalls_before = host.stalls;
    const int host_steps_before = host.steps;
    for (int f = 0; f < 40; ++f) {
        pump(all, 3, host.s);
        host.step();
        if (a.s->live()) a.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check("the host waits for a stalled member instead of dropping it",
          host.steps - host_steps_before <= 3 &&
              host.stalls - host_stalls_before >= 35);
    check("the host names who it is waiting on", host.s->waiting_on() == 2);
    int max_gap = 0;
    for (int f = 0; f < 60; ++f) {
        pump(all, 3, host.s);
        host.step();
        if (a.s->live()) a.step();
        if (b.s->live()) b.step();
        const int gap_a = int(host.s->recv_cursor()) - int(a.s->recv_cursor());
        const int gap_b = int(host.s->recv_cursor()) - int(b.s->recv_cursor());
        max_gap = std::max(max_gap, std::max(gap_a, gap_b));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("    largest host-to-client dataset gap after the stall: %d\n",
                max_gap);
    check("no console trails the host by the checksum history's depth",
          max_gap <= host.s->delay() + 2 && max_gap < 8);
    {
        /* From B's first present dataset on, B is in every one. */
        bool seen_b = false, hole = false;
        for (const auto& [index, d] : host.seen) {
            if (d.present & 0x4) seen_b = true;
            else if (seen_b && (d.members & 0x4)) hole = true;
        }
        check("once sharing, B is present in every dataset (no holes)",
              seen_b && !hole);
    }
    check("host and B agree across the stall", agree(host, b, &compared));
    check("A and B agree across the stall", agree(a, b, &compared));

    /* A HANGS: connected, but it never sends again. The host waits the stall
     * limit (kLsw2VwhStallSteps host steps - the real one, not a test value),
     * then kicks it, as WM's timeout would, and carries on with B. */
    const int host_steps_hang = host.steps;
    for (int f = 0; f < int(kLsw2VwhStallSteps) + 60; ++f) {
        pump(all, 3, host.s);
        host.step();
        if (b.s->live()) b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        const Dataset& last = host.seen.rbegin()->second;
        check("a hung member is kicked and the others carry on",
              last.members == 0x5 && last.present == 0x5 &&
                  host.steps - host_steps_hang > 30);
    }
    check("host and B still agree after the kick", agree(host, b, &compared));

    /* THE SESSION DEEPENS AT ONE DATASET: the binding's depth
     * effect has every console raise its pipeline as it consumes the commit
     * that carries the change - here the test's consoles do what the binding
     * does - and the datasets stay identical everywhere across the change. */
    {
        const uint8_t deeper[] = {kDepthEffect, 1, 4};
        check("the host posts a depth change", host.s->post_effect(deeper, sizeof deeper));
        for (int f = 0; f < 90; ++f) {
            pump(all, 3, host.s);
            host.step();
            if (b.s->live()) b.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check("host and B both run the new depth",
              host.s->delay() == 4 && b.s->delay() == 4);
        check("host and B agree across the change", agree(host, b, &compared));
        const Dataset& last = host.seen.rbegin()->second;
        check("and B is still present in every dataset after it",
              last.members == 0x5 && last.present == 0x5);
        host.s->raise_delay(2);
        check("a lower depth is ignored", host.s->delay() == 4);
    }

    /* B leaves: its socket closes, and the host is alone again. */
    b.t.close();
    for (int f = 0; f < 40; ++f) {
        pump(all, 3, host.s);
        host.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        const Dataset& last = host.seen.rbegin()->second;
        check("B is out of the membership after leaving, host alone again",
              last.members == 0x1 && last.present == 0);
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures,
                g_failures == 1 ? "" : "s");
    delete host.s;
    delete a.s;
    delete b.s;
    return g_failures ? 1 : 0;
}
