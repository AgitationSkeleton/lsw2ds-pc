/* See masterlist.h. */

#include "masterlist.h"

#include <cstdio>
#include <cstring>

namespace {

/* PING BOUNDARIES, AND THEY ARE GUESSES. Nothing has measured what this game
 * feels like at a given latency - the lockstep stalls rather than mispredicts,
 * so the honest answer is "it feels like the worst connection in the room" and
 * that is not a number. Round figures, in one place, to be replaced by
 * something measured rather than argued about. */
constexpr int kGoodMs = 60;
constexpr int kFairMs = 120;

}  // namespace

Lsw2Signal lsw2_signal_for_ping(int ping_ms) {
    /* UNMEASURED IS NOT THE SAME AS BAD. -1 means nobody has pinged this host
     * yet, and showing that as an empty meter would say "unreachable" about a
     * lobby that may be next door. */
    if (ping_ms < 0) return Lsw2Signal::None;
    if (ping_ms <= kGoodMs) return Lsw2Signal::Good;
    if (ping_ms <= kFairMs) return Lsw2Signal::Fair;
    return Lsw2Signal::Weak;
}

void lsw2_master_row_format(const Lsw2ServerEntry& entry, Lsw2MasterRow* out) {
    if (!out) return;

    /* THE SPECIFIED ORDER, and its example is the test: name, count out of four,
     * ping. "Maclunkey 1/4 96". The icon follows as a sprite, so it is a value
     * here rather than a character. */
    const int max_players = entry.max_players > 0 ? entry.max_players : 4;

    if (entry.ping_ms >= 0) {
        std::snprintf(out->text, sizeof out->text, "%s %d/%d %d",
                      entry.name.c_str(), entry.players, max_players,
                      entry.ping_ms);
    } else {
        /* No measurement yet. A dash says so; a zero would claim a perfect
         * connection to a host nobody has spoken to. */
        std::snprintf(out->text, sizeof out->text, "%s %d/%d -",
                      entry.name.c_str(), entry.players, max_players);
    }

    out->signal = lsw2_signal_for_ping(entry.ping_ms);
    out->address = entry.address;
    out->port = entry.port;
    out->lan = entry.lan;
}

void lsw2_master_list_set(Lsw2MasterListState& state,
                          const std::vector<Lsw2ServerEntry>& servers,
                          bool directory_reachable,
                          bool lan_first, int max_rows) {
    if (max_rows < 0) max_rows = 0;
    state.rows.clear();
    state.total = int(servers.size());
    state.note = nullptr;
    state.lan_first = lan_first;

    /* THE PARTITION HAPPENS BEFORE THE CAP, and that is the whole point of
     * doing it here rather than sorting the finished rows.
     *
     * The list holds eleven and the directory can return more. Ordering after
     * the cap would let eleven internet lobbies push a LAN game - the one on the
     * same desk as the player, the one that always works - off the end of a list
     * that it should be at the TOP of. */
    std::vector<const Lsw2ServerEntry*> ordered;
    ordered.reserve(servers.size());
    if (lan_first)
        for (const Lsw2ServerEntry& e : servers)
            if (e.lan) ordered.push_back(&e);
    for (const Lsw2ServerEntry& e : servers)
        if (!lan_first || !e.lan) ordered.push_back(&e);

    for (const Lsw2ServerEntry* e : ordered) {
        if (int(state.rows.size()) >= max_rows) break;
        Lsw2MasterRow row;
        lsw2_master_row_format(*e, &row);
        state.rows.push_back(row);
    }

    /* AN EMPTY LIST HAS TWO CAUSES AND THEY NEED DIFFERENT ACTIONS. Nobody is
     * hosting - wait, or host one yourself. Or the directory is unreachable -
     * check the address on the Multiplayer tab. A browser that shows the same
     * blank screen for both sends the player looking in the wrong place. */
    if (state.rows.empty())
        state.note = directory_reachable ? "No lobbies are open."
                                         : "Cannot reach the master server.";
}

void lsw2_master_list_build(Lsw2Menu& menu, const Lsw2MasterListState& state) {
    menu.reset("Master Server");

    for (const Lsw2MasterRow& r : state.rows) {
        Lsw2MenuRow m;
        m.label = r.text;
        /* THE FOURTH PART OF THE LOBBY ROW, finally connected. `signal` has been
         * computed by lsw2_master_row_format since the browser was written and
         * read by nothing - the row's text carried name, count and ping, and
         * the icon had nowhere to go. `badge` is that somewhere.
         *
         * None maps to 0 so an unpinged host draws no bars at all, which is the
         * same distinction the text makes with "-": unmeasured is not the same
         * as bad. */
        m.badge = int(r.signal);
        /* LAN lobbies are highlighted in green text at the top of the list -
         * the colour is the other half of that rule, and it is applied only
         * when the caller asked for this presentation. The fork's browser
         * passes lan_first = false and its LAN rows draw like any other, which
         * is what the master server's specification says they should. */
        if (state.lan_first && r.lan) m.colour = kLsw2MenuGreen;
        menu.add(m);
    }

    if (state.note) {
        Lsw2MenuRow m;
        m.label = state.note;
        m.selectable = false;    /* information, not a choice */
        menu.add(m);
    }

    Lsw2MenuRow back;
    back.label = "Return";
    menu.add(back);
}

int lsw2_master_list_pick(const Lsw2MasterListState& state, int row) {
    if (row < 0 || row >= int(state.rows.size())) return -1;
    return row;
}
