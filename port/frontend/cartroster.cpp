/* See cartroster.h - the guest half. */

#include "cartroster.h"

#include "coopjoin.h"        /* kLsw2WirelessOwner */
#include "forksession.h"
#include "netcount.h"        /* kLsw2NetCountObject, kLsw2NetPlayerCount, ... */
#include "whsession.h"       /* lsw2_wh_role_is_host */
#include "../net/lsw2_name.h"
#include "../net/lsw2_net_bind.h"   /* lsw2_vwh_enabled */
#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

uint32_t rd32(uint32_t addr) { return bus_device_read32(0, addr); }

uint8_t rd8(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

bool in_ram(uint32_t a) { return a >= kRamLow && a < kRamHigh; }

/* THE STATIONS THIS MODULE ADDED, and only those. The TCP seat path owns
 * console_.peers too; tracking our own set means a peer that arrived over a
 * socket is never removed by us, and a cartridge peer is added exactly once. */
int g_known[kLsw2CartMaxSlots];
int g_known_count = 0;

bool is_known(int station) {
    for (int i = 0; i < g_known_count; ++i)
        if (g_known[i] == station) return true;
    return false;
}

void forget(int station) {
    for (int i = 0; i < g_known_count; ++i) {
        if (g_known[i] == station) {
            for (int j = i + 1; j < g_known_count; ++j) g_known[j - 1] = g_known[j];
            --g_known_count;
            return;
        }
    }
}

bool g_said_conflict = false;
bool g_synced = false;    /* have we completed one tick already in this session */

/* THE SEATING MUST HOLD STILL BEFORE IT IS READ. Measured:
 * during admission the net-count object's seat count and LOCAL STATION flap for
 * a handful of frames - the joiner briefly reads its own station as 0, so its
 * own seat (station 1) looks like a remote peer and gets announced, then
 * corrected. Reading the record mid-admission is reading garbage. So the feed
 * waits until (seat_count, local_station) have been unchanged for this many
 * frames, the same settling-window idea the portrait fix uses. */
constexpr int kSettleFrames = 30;
int g_settle = 0;
int g_last_seat = -1;
int g_last_local = -1;

}  // namespace

void lsw2_cartroster_tick() {
    if (!ui_fork_build()) return;
    /* THE RADIO ROUTE'S FEED, and only that route's. With the host-level WH
     * (the fork's default) every peer arrives through the transport, which
     * seats, names and announces it; the cartridge's seat record is a second
     * copy of the same facts. Measured 2026-09-23 (conn_kick1): a kicked
     * console ended its session, and this then re-added the two stations the
     * cartridge had not torn down yet and announced each leaving - three
     * "has disconnected" popups for one kick. */
    if (lsw2_vwh_enabled()) return;

    /* The cartridge's seat count. One or zero means no remote peer is seated
     * through the radio, so there is nothing to feed and any peer we had is
     * gone. */
    const int seat_count = rd8(kLsw2NetCountObject + kLsw2NetPlayerCount);
    const int local_station = rd8(kLsw2NetCountObject + kLsw2NetLocalStation);
    const bool is_host = lsw2_wh_role_is_host();

    /* A WH CHILD IS NEVER STATION 0 - the host is aid 0, children are aid 1+.
     * So a JOINER reading its local station as 0 has not been seated yet, and
     * the record it would read still shows its own future seat as a stranger
     * (measured 2026-09-13: the joiner announced itself, then the host, as the
     * reading corrected). Treat that as unsettled: hold the window open until a
     * joiner's station is a real child index. The host, correctly station 0,
     * is unaffected. */
    const bool station_unseated =
        (!is_host && seat_count > 1 &&
         (local_station < 1 || local_station >= kLsw2CartMaxSlots));

    /* WAIT FOR THE SEATING TO SETTLE before trusting the record. A change in
     * either the count or the local station re-arms the window; only once both
     * have held for kSettleFrames - and the local station is real - is the read
     * believed. The Disconnect mark below is safe to set from an unsettled
     * count because it only cares whether a session exists at all, but the peer
     * feed - which names and announces - waits. */
    if (seat_count != g_last_seat || local_station != g_last_local ||
        station_unseated) {
        g_last_seat = seat_count;
        g_last_local = local_station;
        g_settle = kSettleFrames;
    } else if (g_settle > 0) {
        --g_settle;
    }

    /* THE JOINER'S SESSION IS MARKED, so its pause row reads "Disconnect".
     * A child seated with a host (seat_count >= 2, and we are not the host) is
     * in a session even though it never went through the fork's own join path.
     * The host's own state is already Hosting and set_cartridge_child leaves it
     * untouched. When the seat empties, the mark is lifted. */
    Lsw2ForkSession& session = lsw2_fork_session();
    session.set_cartridge_child(!is_host && seat_count > 1);

    /* The peer feed only runs on a settled seating. Until then, leave whatever
     * is known alone - a half-read record must not add or drop a peer. */
    if (g_settle > 0) return;

    /* Read the record and its four name slots. A missing pointer clears our
     * set the same way an empty seat count does. */
    Lsw2CartSlot slots[kLsw2CartMaxSlots] = {};
    const uint32_t rec = in_ram(kLsw2WirelessOwner)
                             ? rd32(kLsw2WirelessOwner + kLsw2CartRosterPtrOffset)
                             : 0u;
    if (in_ram(rec) && seat_count > 1) {
        for (int s = 0; s < kLsw2CartMaxSlots; ++s) {
            char raw[kLsw2CartNameStride + 1];
            const uint32_t base = rec + kLsw2CartNameOffset +
                                  uint32_t(s) * kLsw2CartNameStride;
            int len = 0;
            for (uint32_t i = 0; i < kLsw2CartNameStride; ++i) {
                const char c = char(rd8(base + i));
                if (c == '\0') break;
                raw[len++] = c;
            }
            raw[len] = '\0';
            slots[s].present = (len > 0);
            if (slots[s].present)
                lsw2_sanitise_name(raw, slots[s].name, sizeof slots[s].name,
                                   "Player");
            else
                slots[s].name[0] = '\0';
        }
    }

    const char* names[3] = {nullptr, nullptr, nullptr};
    int stations[3] = {-1, -1, -1};
    const int n = lsw2_cartroster_remote_peers(slots, local_station, seat_count,
                                               names, stations);

    /* DO NOT DOUBLE-COUNT A TCP SESSION. If the session already holds peers we
     * did not add, a socket seated them and the cartridge reading would be a
     * second copy. Say so once and leave it to the transport. */
    if (session.player_count() - 1 > g_known_count) {
        if (!g_said_conflict) {
            g_said_conflict = true;
            std::fprintf(stderr,
                         "[fork] cartroster: the session already has peers from "
                         "the transport; not feeding the cartridge's record on "
                         "top of them\n");
        }
        return;
    }

    /* ADD the remote peers we do not yet know. peer_joined raises the join
     * popup and puts the name in the kick list.
     *
     * NO "HAS JOINED" ON THE FIRST SYNC. A peer already seated when this module
     * first looks was there before we started watching - for a JOINER that is
     * the host it just connected to, which is not a "joined" event to announce.
     * So the first tick records peers silently (announce=false) and only later
     * arrivals pop up. A host's first tick is alone (no peers), so its real
     * first joiner arrives on a later tick and is announced, as it should be. */
    /* A HOST always announces; a JOINER stays silent about the peers already
     * seated when it arrived (the host it just connected to) - g_synced is
     * false on its first settled tick, having reset while it was alone. */
    const bool announce = is_host || g_synced;
    for (int i = 0; i < n; ++i) {
        if (is_known(stations[i])) continue;
        if (g_known_count >= kLsw2CartMaxSlots) break;
        session.peer_joined(names[i], stations[i], announce);
        g_known[g_known_count++] = stations[i];
        std::fprintf(stderr,
                     "[fork] cartroster: %s %s at station %d (from the "
                     "cartridge's own seat, not a socket)\n",
                     names[i], announce ? "joined" : "was already here",
                     stations[i]);
    }
    g_synced = true;

    /* REMOVE the ones that are no longer seated. peer_left_at raises the
     * disconnect popup. Walk a copy, because forget() edits g_known. */
    int snapshot[kLsw2CartMaxSlots];
    const int m = g_known_count;
    for (int i = 0; i < m; ++i) snapshot[i] = g_known[i];
    for (int i = 0; i < m; ++i) {
        bool still = false;
        for (int j = 0; j < n; ++j)
            if (stations[j] == snapshot[i]) { still = true; break; }
        if (still) continue;
        const int row = session.peer_row_for_station(snapshot[i]);
        if (row >= 0) session.peer_left_at(row);
        forget(snapshot[i]);
        std::fprintf(stderr,
                     "[fork] cartroster: station %d left (its cartridge seat is "
                     "empty now)\n",
                     snapshot[i]);
    }

    /* SESSION OVER, RE-ARM THE FIRST-SYNC RULE. Once no cartridge peer remains
     * and the seat count is back to one, a later session must announce its
     * peers afresh - a joiner who leaves and rejoins should not have the host
     * suppressed a second time. */
    if (seat_count <= 1 && g_known_count == 0) g_synced = false;
}
