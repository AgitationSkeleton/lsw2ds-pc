/* See forksession.h. */

#include "forksession.h"

#include "coophosts.h"

#include "forkpopup.h"
#include "lobbyrespawn.h"
#include "menuhost.h"
#include "roster.h"
/* lsw2_wh_console_join - the cartridge's own browse list, opened by the
 * player's choice at the arena console rather than because boot aimed us there. */
#include "whsession.h"
#include "../net/lsw2_name.h"
#include "../net/lsw2_ping.h"
#include "../net/lsw2_upnp.h"
#include "../net/lsw2_net_bind.h"
#include "../net/lsw2_relay_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
/* The fork starts the wireless relay itself for internet play, deriving
 * every param from the session (base, AIDs, ports, peer address) instead of the
 * player setting LSW2_RELAY*. Behind ONE launcher-set master switch so ordinary
 * same-machine local play (which shares the wireless base and hears itself
 * directly) never binds the relay and double-delivers. When set, LSW2_WIRELESS_BASE
 * (== --local-wireless-port) tells the fork the wireless base; default 26710 is
 * the runner's own default (frontend.h/wifi_net.h), so two machines that both take
 * the default agree without any extra flag. */
bool lsw2_relay_auto_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_RELAY_AUTO");
        return e && *e == '1';
    }();
    return on;
}
int lsw2_relay_wireless_base() {
    if (const char* e = std::getenv("LSW2_WIRELESS_BASE")) {
        const int v = std::atoi(e);
        if (v >= 1024 && v <= 65520) return v;
    }
    return 26710;  /* the runner's default --local-wireless-port */
}
}  // namespace

namespace {
Lsw2ForkSession g_session;

/* HOW LONG A JOIN WAITS FOR ITS SEAT, in 5ms pumps - one second. The host sends
 * the assignment the moment it accepts the socket, so this is generous by
 * orders of magnitude; it is a bound so a host that accepts and then says
 * nothing fails the join instead of hanging the frame. See join_address(). */
constexpr int kJoinSeatSpins = 200;

/* HOW LONG THE WHOLE PING SWEEP MAY COST, on top of the directory refresh the
 * player has already waited for. Six hundred milliseconds is long enough to
 * reach anywhere on the same continent and short enough that a browser full of
 * unreachable lobbies still opens promptly - and an unmeasured row reads as
 * "no signal", which is honest rather than wrong. */
constexpr int kPingBudgetMs = 600;

/* Consecutive list refreshes a LAN host may go unheard and still be kept; it
 * is dropped on the next one after these. See the expire() call in
 * refresh_lobbies for why the slack is here at all. */
constexpr int kLanMissedRefreshes = 2;
}  // namespace

Lsw2ForkSession& lsw2_fork_session() { return g_session; }

Lsw2Menu* Lsw2ForkSession::menu() {
    return screen_ == Lsw2ForkScreen::None ? nullptr : &menu_;
}

void Lsw2ForkSession::rebuild_menu() {
    switch (screen_) {
    case Lsw2ForkScreen::ArenaMenu:
        lsw2_arena_menu_build(menu_);
        break;
    case Lsw2ForkScreen::JoinCode:
        lsw2_join_menu_build(menu_, join_);
        lsw2_join_menu_sync(menu_, join_, direct_host_, direct_port_);
        break;
    case Lsw2ForkScreen::MasterList:
        lsw2_master_list_build(menu_, list_);
        break;
    case Lsw2ForkScreen::HostSettings:
        lsw2_host_menu_build(menu_, settings_);
        lsw2_host_menu_sync(menu_, settings_);
        /* THE LAST ROW SAYS WHAT IT WILL DO. Reached from the lobby console
         * while hosting, the lobby is already up and the row applies the
         * changed settings to it (start_lobby re-announces them), so there
         * "Start Lobby" reads "Update Lobby". */
        if (is_host()) menu_.row(kHostStart).label = "Update Lobby";
        break;
    case Lsw2ForkScreen::Extras:
        lsw2_extras_menu_build(menu_, settings_);
        break;
    case Lsw2ForkScreen::Character:
        /* NO MENU: the cartridge's Free Play grid on the touch screen is the
         * picker (freeplaypick.h), and a fork menu would own that screen and
         * swallow its taps. The one-character picker uses the Free Play grid
         * in place of an individual character list. */
        lsw2_menuhost_close();
        return;
    case Lsw2ForkScreen::Console:
        lsw2_console_menu_build(menu_, console_);
        lsw2_console_menu_sync(menu_, console_);
        break;
    case Lsw2ForkScreen::Kick:
        if (!lsw2_kick_menu_build(menu_, console_)) {
            /* Kick Player lists the players besides the host - with nobody
             * else connected there is no list, so fall back rather than show
             * an empty one. */
            screen_ = Lsw2ForkScreen::Console;
            rebuild_menu();
            return;
        }
        break;
    case Lsw2ForkScreen::None:
        lsw2_menuhost_close();
        return;
    }
    lsw2_menuhost_open(&menu_);
}

void Lsw2ForkSession::open_arena_menu() {
    screen_ = Lsw2ForkScreen::ArenaMenu;
    parent_ = Lsw2ForkScreen::None;
    rebuild_menu();
}

void Lsw2ForkSession::open_host_settings() {
    screen_ = Lsw2ForkScreen::HostSettings;
    rebuild_menu();
}

void Lsw2ForkSession::open_join_by_code() {
    screen_ = Lsw2ForkScreen::JoinCode;
    parent_ = Lsw2ForkScreen::ArenaMenu;
    rebuild_menu();
}

void Lsw2ForkSession::refresh_lobbies(bool lan_first) {
    /* THE FETCH, SPLIT OUT SO THERE IS ONE OF IT.
     *
     * `open_master_list()` did this and then opened the fork's own browser. The
     * baseline needs the same fetch and a different screen - the cartridge's
     * own lobby list, which the baseline fills from the master server and the
     * LAN - and two copies of a directory fetch is
     * two things to keep in step. The same reason the round start has one
     * implementation and two triggers. */

    /* FETCH BEFORE OPENING, so the screen is never briefly empty and then
     * populated - a list that fills in under the cursor moves the row the
     * player was about to choose. */
    const bool ok = master_.refresh();

    /* MEASURE THE PING BEFORE THE LIST IS BUILT.
     *
     * A lobby row reads "Maclunkey 1/4 96 <green_ds_icon>" - the number and the
     * icon
     * are both derived from a round trip this client makes, because a ping from
     * the directory would be the distance to the DIRECTORY. `ping_ms` had
     * existed since the master client was written and nothing ever assigned it,
     * so every row showed no number and no icon.
     *
     * The player is already waiting on the refresh above, so this is the moment
     * that costs them least - and the sweep probes every host at once, so it
     * costs one budget rather than one per lobby. */
    std::vector<Lsw2ServerEntry> rows = master_.servers();
    {
        std::vector<Lsw2PingTarget> targets;
        targets.reserve(rows.size());
        for (const Lsw2ServerEntry& e : rows)
            targets.push_back({e.address, e.port, -1});

        lsw2_ping_sweep(targets, kPingBudgetMs);
        for (size_t i = 0; i < rows.size() && i < targets.size(); ++i)
            rows[i].ping_ms = targets[i].ping_ms;
    }

    /* AND THE LOCAL NETWORK. LAN game support means scanning the local
     * network the player is on for games hosted there, and until this was wired
     * the fork only ever announced - the browser existed and nothing ran it.
     *
     * Started here rather than at boot: a socket bound for a screen nobody has
     * opened is a socket held for nothing. Two polls with a short gap, because
     * hosts announce about once a second and one poll would usually be empty. */
    if (!browser_.running()) browser_.start();
    if (browser_.running()) {
        browser_.ignore(lan_.id());     /* not our own announcement */
        browser_.poll();
        /* AND DROP THE HOSTS THAT HAVE GONE. A beacon that stops - the host
         * quit, or started the match - says nothing on the way out, so without
         * this its row stays in the browser for the rest of the session and the
         * player joins a lobby that is not there.
         *
         * A ROW SURVIVES TWO MISSES AND GOES ON THE THIRD - the test is
         * `last_seen_ > limit`, so the limit is the number tolerated, not the
         * number that kills. The socket buffers between refreshes, so a live
         * host announcing once a second is heard on every refresh at least a
         * second after the last; the slack is for refreshes pressed in quick
         * succession, which can straddle a single announce interval. Dropping a
         * live host because the player pressed refresh twice would be worse
         * than carrying a dead one for one more press. */
        browser_.expire(kLanMissedRefreshes);
        for (const Lsw2ServerEntry& lan : browser_.servers()) {
            bool already = false;
            for (const Lsw2ServerEntry& e : rows)
                if (e.address == lan.address && e.port == lan.port)
                    already = true;
            /* A HOST VISIBLE BOTH WAYS IS ONE LOBBY. Somebody hosting with both
             * "Visible to Master" and "Visible to LAN" on appears in both
             * sources, and listing them twice would look like two games. */
            if (already) continue;
            /* MARKED AS LOCAL. The base recomp's list shows LAN games at the
             * top in green, and until the entry carried this it was
             * indistinguishable once appended. The fork does not reorder or
             * recolour - its LAN rows are mixed into the list - so the flag
             * travels and the presentation does not. */
            Lsw2ServerEntry local = lan;
            local.lan = true;
            rows.push_back(local);
        }
    }

    /* THE CARTRIDGE'S LIST HOLDS SIXTEEN and the fork's browser eleven, so the
     * cap follows the screen rather than the fetch - see masterlist.h. The
     * baseline asks for lan_first, and that is the same request. */
    lsw2_master_list_set(list_, rows, ok || master_.reachable(), lan_first,
                         lan_first ? kLsw2HostSlots : kLsw2MasterListMax);

    /* SAY WHEN THE LIST IS SHORTER THAN THE TRUTH. Silently showing eleven of
     * twenty lobbies reads as "that is all there is". */
    if (list_.total > int(list_.rows.size()))
        std::fprintf(stderr, "[fork] master list shows %d of %d lobbies\n",
                     int(list_.rows.size()), list_.total);

}

int Lsw2ForkSession::list_row_count() const {
    return int(list_.rows.size());
}

void Lsw2ForkSession::open_master_list() {
    refresh_lobbies(/*lan_first=*/false);
    screen_ = Lsw2ForkScreen::MasterList;
    parent_ = Lsw2ForkScreen::ArenaMenu;
    rebuild_menu();
}

bool Lsw2ForkSession::open_console() {
    /* HOST ONLY. A client walking into the console gets nothing - every option
     * on it is an authority a client does not have, and a menu that opens and
     * then refuses each choice is worse than one that does not open. */
    if (!is_host() && state_ != Lsw2SessionState::Offline) return false;
    screen_ = Lsw2ForkScreen::Console;
    rebuild_menu();
    return true;
}

void Lsw2ForkSession::close_screen() {
    screen_ = Lsw2ForkScreen::None;
    parent_ = Lsw2ForkScreen::None;
    lsw2_menuhost_close();
}

/* THE FORK ADVERTISES ARENA MATCHES. The directory holds both kinds - the base
 * recomp's co-op lobbies share it - and the browser here must not list them.
 *
 * In the CONSTRUCTOR, not configure(), because configure() runs from the imgui
 * tab's draw and a player who never opens that tab would otherwise leave the
 * client unlabelled and unfiltered.
 *
 * BUT THE CONSTRUCTOR CANNOT KNOW WHICH BUILD THIS IS. This is one static
 * object shared by both launchers, and `ui_set_fork_build()` runs later - so
 * hard-coding `bounty` here was right only while the baseline had no networking
 * at all. It now has co-op networking, and a baseline co-op
 * lobby advertised as `bounty` would appear in the fork's browser and be joined
 * by players expecting an arena. `bounty` stays the DEFAULT, because a fork that
 * never announced its build would still be labelled correctly; `set_coop()` is
 * what the baseline calls, once, from the same place the build flag is set. */
Lsw2ForkSession::Lsw2ForkSession() {
    master_.set_kind(LSW2_KIND_BOUNTY);
}

bool Lsw2ForkSession::take_start_request() {
    const bool want = start_requested_;
    start_requested_ = false;
    return want;
}

bool Lsw2ForkSession::lobby_tick(uint32_t now, bool hold) {
    Lsw2LobbyTimerInput in;
    /* is_host(), NOT state_ == Hosting - the same reason shut_down_lobby()
     * gives. A host is still the host during a match, and in_match below is what
     * distinguishes the lobby from the round rather than the state alone. */
    in.hosting = is_host();
    in.in_match = (state_ == Lsw2SessionState::InMatch);
    in.players = players_present();
    in.lobby_seconds = settings_.lobby_seconds();
    in.now = now;
    in.hold = hold;
    return lobby_.tick(in);
}

int Lsw2ForkSession::lobby_remaining(uint32_t now) const {
    Lsw2LobbyTimerInput in;
    in.hosting = is_host();
    in.in_match = (state_ == Lsw2SessionState::InMatch);
    in.players = players_present();
    in.lobby_seconds = settings_.lobby_seconds();
    in.now = now;
    return lobby_.remaining(in);
}

int Lsw2ForkSession::known_lobbies() const {
    return int(list_.rows.size());
}

void Lsw2ForkSession::lobby_name(int row, char* out, int n) const {
    if (!out || n <= 0) return;
    out[0] = 0;   /* an empty name, without a NUL literal in the source */
    if (row < 0 || row >= int(list_.rows.size())) return;
    /* THE ROW TEXT, which already carries name, count and ping in the specified
     * order and has already been through the sanitiser. Re-deriving a name here
     * would be a second format to keep in step with the first. */
    std::snprintf(out, size_t(n), "%s", list_.rows[size_t(row)].text);
}

bool Lsw2ForkSession::join_row(int row) {
    if (row < 0 || row >= int(list_.rows.size())) return false;
    const Lsw2MasterRow& r = list_.rows[size_t(row)];
    if (r.address.empty() || r.port <= 0) return false;
    return join_address(r.address.c_str(), r.port);
}

void Lsw2ForkSession::publish_parts(const uint16_t* parts, int count) {
    if (transport_) transport_->send_parts(parts, count);
}

bool Lsw2ForkSession::take_parts(int* station, uint16_t* out, int count) {
    return transport_ && transport_->take_parts(station, out, count);
}

void Lsw2ForkSession::set_coop(bool coop) {
    master_.set_kind(coop ? LSW2_KIND_COOP : LSW2_KIND_BOUNTY);
}

void Lsw2ForkSession::configure(const char* name, const char* master_host,
                                int master_port, int listen_port) {
    /* The local name is the player's own, so this is hygiene rather than
     * defence - but it is the name that goes to the master server and into the
     * game's own "<name> - Arena" room title, and a stray control character
     * there would travel just as far as a hostile one. */
    if (name && *name)
        lsw2_sanitise_name(name, player_name_, sizeof player_name_, "Maclunkey");
    if (master_host && *master_host) master_.configure(master_host, master_port);
    if (listen_port > 0) listen_port_ = listen_port;

}

void Lsw2ForkSession::configure_direct(const char* host, int port) {
    if (host) std::snprintf(direct_host_, sizeof direct_host_, "%s", host);
    if (port > 0) direct_port_ = port;
}

void Lsw2ForkSession::update_lobby() {
    if (!is_host()) return;

    /* LAN VISIBILITY, turned on or off as start_lobby() would have set it. */
    if (settings_.visible_lan && !lan_.running()) {
        if (lan_.start(player_name_, uint16_t(listen_port_)))
            lan_.announce(player_count(), kLsw2MaxStations);
    } else if (!settings_.visible_lan && lan_.running()) {
        lan_.stop();
    }

    /* MASTER VISIBILITY. A lobby already listed keeps its room code and its
     * heartbeat; one newly made visible is registered as start_lobby() does;
     * one hidden again is withdrawn. */
    const bool listed = console_.room_code[0] != '\0';
    if (settings_.visible_master && !listed) {
        char room[64];
        std::snprintf(room, sizeof room, "%s", player_name_);
        publish_roster();
        if (master_.register_lobby(room, listen_port_, player_count()))
            std::snprintf(console_.room_code, sizeof console_.room_code, "%s",
                          master_.room_code().c_str());
    } else if (!settings_.visible_master && listed) {
        master_.unregister();
        console_.room_code[0] = '\0';
    }

    /* AND THE RULES, to every seated player: clients adopt the host's roster,
     * customization and extras from this (take_settings in tick). */
    broadcast_settings();

    std::fprintf(stderr,
                 "[fork] lobby updated in place (roster %d, lobby %ds, LAN %s, "
                 "master %s) - %d player(s) stay seated\n",
                 int(settings_.roster), settings_.lobby_seconds(),
                 settings_.visible_lan ? "on" : "off",
                 console_.room_code[0] ? console_.room_code : "off",
                 player_count());
}

void Lsw2ForkSession::start_lobby() {
    transport_ = std::make_unique<Lsw2TcpTransport>();
    const uint16_t bound = transport_->host(uint16_t(listen_port_));
    if (bound == 0) {
        transport_.reset();
        last_error_ = "could not listen - is the port already in use?";
        std::fprintf(stderr, "[fork] %s\n", last_error_);
        return;
    }
    listen_port_ = bound;

    /* THIS CONSOLE IS THE HOST. Set the wireless role here, in the
     * common host path, so it is HOST for every way of hosting - not only the
     * console's Start Lobby - and a second window that joins takes the opposite
     * role instead of both defaulting to host and never connecting. */
    lsw2_wh_set_role(true);

    /* THE HOST IS STATION 0, which is also the AID the guest gives itself.
     * It was established that the connected-station bitmap is indexed by AID
     * and that the session's station indices are its bits, so the two numbering
     * schemes are the same one. */
    if (lsw2_vwh_enabled()) {
        /* The game's wireless manager is answered from this session;
         * the Path A lockstep would only compete for the same block records. */
        vwh_ = std::make_unique<Lsw2VwhSession>(transport_.get(), /*is_host=*/true);
        lsw2_vwh_set_session(vwh_.get());
    } else {
        lockstep_ = std::make_unique<Lsw2LockstepSession>(Lsw2NetRole::Host, 0,
                                                          transport_.get());
        lockstep_->seat(0, player_name_, true);
        lsw2_net_set_active_session(lockstep_.get(), 0);
    }
    local_station_ = 0;
    /* Joiners learn the host's name from this; it is re-sent on each accept
     * below, because a peer that connects later missed this one. */
    transport_->send_name(player_name_);

    was_host_ = true;
    state_ = Lsw2SessionState::Hosting;
    console_.peer_count = 0;
    console_.room_code[0] = '\0';

    /* Start Lobby announces the new lobby to the master server, whose list
     * then shows it as a row: the player's name, then how many of the 4 seats
     * are taken.
     *
     * The room name is the GAME's own: the cartridge builds
     * "<player name> - Arena" through the @1 token, so the browser and the game
     * agree rather than the port inventing a second naming scheme. */
    /* THE LAN BEACON, if the host asked to be visible there.
     *
     * Independent of the master server on purpose: the host settings menu has
     * them as two separate rows, and they fail for different reasons. A player with no
     * internet still hosts to their own network, and a directory outage does
     * not take LAN play with it. */
    if (settings_.visible_lan) {
        if (lan_.start(player_name_, uint16_t(listen_port_)))
            lan_.announce(player_count(), kLsw2MaxStations);
        else
            std::fprintf(stderr,
                         "[fork] LAN visibility unavailable (no broadcast)\n");
    }

    /* AUTOMATIC PORT FORWARDING through miniupnpc: a freshly installed copy
     * behind a home router should be able to host with no manual port
     * forwarding by the player.
     *
     * SOFT IN EVERY DIRECTION. No gateway, UPnP switched off, carrier-grade NAT
     * - all of them mean hosting continues without a mapping, which still works
     * over LAN and for anyone who can reach this machine directly. The player is
     * told which happened rather than left to guess why nobody joins. */
    {
        std::string external, why;
        if (lsw2_upnp_map_port(listen_port_, &external, &why)) {
            upnp_mapped_ = true;
            std::fprintf(stderr, "[fork] forwarded port %d via UPnP%s%s\n",
                         listen_port_, external.empty() ? "" : "; external IP ",
                         external.empty() ? "" : external.c_str());
        } else {
            std::fprintf(stderr,
                         "[fork] no automatic port forwarding (%s) - LAN and "
                         "direct connections still work\n", why.c_str());
        }
    }

    /* THE WIRELESS RELAY FOR INTERNET PLAY, started by the fork itself.
     * The host serves on listen_port_+1 and waits for the joiner's link; once
     * linked it carries the DS radio (catch base+joinerAID, inject base+0), so the
     * cartridge's own wireless reaches a remote peer with no external process and
     * no player-set env. Behind LSW2_RELAY_AUTO so ordinary same-machine local
     * play (shared base, direct delivery) never binds the relay; the env
     * LSW2_RELAY path still wins if both are set. The relay link port is
     * UPnP-mapped the same way the lobby port is (soft, like everything here). */
    if (lsw2_relay_auto_enabled()) {
        const int base = lsw2_relay_wireless_base();
        const uint16_t relay_port = uint16_t(listen_port_ + 1);
        std::string rext, rwhy;
        lsw2_upnp_map_port(relay_port, &rext, &rwhy);   /* soft; unmapped at teardown */
        if (lsw2_relay_session_start_explicit(/*is_host=*/true, relay_port,
                                              /*host_addr=*/nullptr, base,
                                              /*local=*/0, /*remote=*/1))
            std::fprintf(stderr,
                         "[fork] relay: serving on link port %u (wireless base %d, "
                         "local AID 0, remote AID 1) for internet play\n",
                         relay_port, base);
        else
            std::fprintf(stderr,
                         "[fork] relay: could not serve on link port %u\n",
                         relay_port);
    }

    if (settings_.visible_master) {
        /* THE PLAYER'S NAME, AND NOTHING APPENDED. The lobby row is the
         * player's name (sanitized), the seats taken of 4, the ping in
         * milliseconds and a DS signal-strength icon, for example
         * "Maclunkey 1/4 96 <green_ds_icon>". A name is what belongs here.
         *
         * IT USED TO SEND "%s - Arena" and the suffix was actively harmful.
         * The directory caps a name at ten characters, so "Maclunkey - Arena"
         * arrived as "Maclunkey " - the cut landing inside the suffix, leaving
         * a trailing space that the log and the in-game list both drew. A
         * longer name would have lost even more of itself to a decoration.
         *
         * AND THE SUFFIX WAS NEVER CARRYING INFORMATION. Which kind of lobby
         * this is travels in its own field - lsw2_master.h's LSW2_KIND_BOUNTY
         * against LSW2_KIND_COOP, set once at construction - and the master
         * server's GUI keeps two separate lists off exactly that. The name was
         * repeating, badly, something the protocol already said. */
        char room[64];
        std::snprintf(room, sizeof room, "%s", player_name_);
        /* Before the registration, not after: a lobby that appeared in the
         * console window with an empty player list until its first heartbeat
         * would read as "nobody has joined" rather than "the host is here". */
        publish_roster();
        if (master_.register_lobby(room, listen_port_, player_count())) {
            std::snprintf(console_.room_code, sizeof console_.room_code, "%s",
                          master_.room_code().c_str());
        } else {
            /* NOT FATAL. A lobby the directory never heard of is still joinable
             * by IP and over LAN, so this reports and carries on rather than
             * refusing to host. */
            last_error_ = "the master server did not accept the lobby";
            std::fprintf(stderr, "[fork] %s: %s\n", last_error_,
                         master_.last_error().c_str());
        }
    }

    std::fprintf(stderr,
                 "[fork] hosting on port %d (roster %d, lobby time %ds)%s%s\n",
                 listen_port_, int(settings_.roster), settings_.lobby_seconds(),
                 console_.room_code[0] ? ", room code " : "",
                 console_.room_code[0] ? console_.room_code : "");

    /* THE LOBBY RESPAWN, AND ONLY WHEN ARMED. Start Lobby did not respawn the
     * player in the lobby the way starting a co-op game does. A co-op start
     * respawns you because it re-enters the level in mode 0x0E, the mode that reaches
     * the spawn dispatcher - see lobbyrespawn.h for the cartridge's own routine
     * and for why three earlier attempts could not have worked.
     *
     * Deliberately the LAST thing start_lobby does. The transport is bound and
     * the session seated by this point, which is the one variable an earlier
     * measurement isolated, so if the request is refused while a TCP session is live this
     * is exactly where that shows.
     *
     * Off unless LSW2_LOBBY_RESPAWN is set: this is the measurement, not the
     * fix, and four guest-reaching changes have been reverted in this project
     * for shipping ahead of one. */
    lsw2_lobby_respawn_arm("start_lobby");
}

bool Lsw2ForkSession::join_address(const char* address, int port) {
    if (!address || !*address) return false;

    transport_ = std::make_unique<Lsw2TcpTransport>();
    state_ = Lsw2SessionState::Joining;
    if (!transport_->join(address, uint16_t(port))) {
        transport_.reset();
        state_ = Lsw2SessionState::Offline;
        last_error_ = "could not reach that host";
        std::fprintf(stderr, "[fork] %s (%s:%d)\n", last_error_, address, port);
        return false;
    }

    /* THIS CONSOLE IS A JOINER. Set the wireless role here, in the
     * common join path, so EVERY join entry point - room code, direct IP, the
     * master list - scans for the host's beacon rather than becoming a second
     * access point. lsw2_wh_console_join sets it too for the console route; this
     * covers the rest. */
    lsw2_wh_set_role(false);

    /* THE STATION COMES FROM THE HOST, not from us. Guessing it would put two
     * clients on the same AID, and their blocks would overwrite each other.
     *
     * AND IT HAS TO BE WAITED FOR, which this did not do. Measured 2026-09-09
     * driving a join by room code: the connection succeeded and the log read
     *
     *     [fork] joined 127.0.0.1:28550 as station -1
     *
     * while the directory showed the host had seated that player in slot 1.
     * lsw2_tcp.h says the assignment is "reported back through
     * assigned_station() ONCE THE HANDSHAKE COMPLETES"; the host sends it on
     * accept and the client records it while consuming records in service().
     * Reading it on the line after join() returns is therefore reading it
     * before it can possibly have arrived - it is -1 every time, not
     * occasionally.
     *
     * WHAT THAT COST. `local_station_` is never corrected afterwards, and
     * tick() uses it to skip the station's own blocks relayed back
     * ("if (station == local_station_) continue"), which at -1 never matches -
     * so this console would treat its own echoes as a peer's. Worse, the
     * transport's own defensive guard writes
     * `is_host_ ? 0 : (assigned_ < 0 ? 0 : assigned_)` into any record sent
     * before the seat arrives, which claims station 0 - THE HOST'S - and is
     * precisely the collision the paragraph above warns about.
     *
     * SO THE SEAT IS WAITED FOR, and a join that never gets one FAILS rather
     * than seating on a number the host did not give. service() is
     * non-blocking socket pumping, so this spins rather than sleeps on a
     * socket; a second is a very long time for a record the host sends on
     * accept, and the alternative to waiting is being silently wrong forever. */
    int station = transport_->assigned_station();
    for (int spin = 0; spin < kJoinSeatSpins && station < 0; ++spin) {
        transport_->service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        station = transport_->assigned_station();
    }
    if (station < 0) {
        transport_.reset();
        state_ = Lsw2SessionState::Offline;
        last_error_ = "the host never sent a seat";
        std::fprintf(stderr, "[fork] %s (%s:%d) - refusing to join on a "
                             "station the host did not assign\n",
                     last_error_, address, port);
        return false;
    }
    if (lsw2_vwh_enabled()) {
        vwh_ = std::make_unique<Lsw2VwhSession>(transport_.get(), /*is_host=*/false);
        lsw2_vwh_set_session(vwh_.get());
        lsw2_vwh_set_host_name("Host");
    } else {
        lockstep_ = std::make_unique<Lsw2LockstepSession>(Lsw2NetRole::Client,
                                                          station, transport_.get());
        lockstep_->seat(station, player_name_, true);
        lsw2_net_set_active_session(lockstep_.get(), station);
    }
    local_station_ = station;
    transport_->send_name(player_name_);

    /* Dial the host's wireless relay for internet play. The host serves
     * on its listen_port+1, so we connect to <address>:(port+1); once linked it
     * carries the DS radio (catch base+0 from the host, inject base+localAID).
     * Behind LSW2_RELAY_AUTO; the env LSW2_RELAY path still wins if set. */
    if (lsw2_relay_auto_enabled()) {
        const int base = lsw2_relay_wireless_base();
        const uint16_t relay_port = uint16_t(port + 1);
        if (lsw2_relay_session_start_explicit(/*is_host=*/false, relay_port,
                                              address, base,
                                              /*local=*/local_station_, /*remote=*/0))
            std::fprintf(stderr,
                         "[fork] relay: dialing %s:%u (wireless base %d, local AID "
                         "%d, remote AID 0) for internet play\n",
                         address, relay_port, base, local_station_);
        else
            std::fprintf(stderr,
                         "[fork] relay: could not dial %s:%u\n", address, relay_port);
    }

    was_host_ = false;
    state_ = Lsw2SessionState::Joined;

    /* COUNT THE HOST AS A PEER. The host is always station 0. take_joined() only
     * reports INCOMING accepts, which the joiner never gets for the host it dialed
     * out to - so without this the joiner's peer_count stays 0, player_count() is
     * 1, and lsw2_viewmask_publish() (gated on player_count()>=2) never runs, so
     * the joiner's game count is stuck at 1 while the HOST correctly reaches 2.
     * Registering the host here makes the two symmetric: the joiner's mask carries
     * both stations (compose(local, [0], 1) -> 0x3) and its own count reaches 2.
     * The host's real name replaces the placeholder when its name record arrives.
     * Guarded so a joiner that somehow got station 0 does not add itself. */
    if (station != 0 && console_.peer_count == 0)
        peer_joined("Host", 0, /*announce=*/false);

    std::fprintf(stderr, "[fork] joined %s:%d as station %d (peer_count %d, "
                 "player_count %d)\n", address, port, station,
                 console_.peer_count, player_count());
    return true;
}

bool Lsw2ForkSession::join_code(const char* code) {
    if (!code || !*code) return false;
    Lsw2ServerEntry entry;
    if (!master_.resolve(code, &entry)) {
        /* The browser has to tell these apart, so the session does
         * too: an unknown code is a typo, an unreachable directory is ours. */
        last_error_ = master_.reachable() ? "no lobby with that code"
                                          : "cannot reach the master server";
        std::fprintf(stderr, "[fork] %s\n", last_error_);
        return false;
    }
    /* Internet play goes over the real WH carried by the relay, NOT Path A
     * (measured): Path A's lobby/roster/seed fights the real WH and resets the
     * match to -1. Under LSW2_RELAY_AUTO the resolved peer feeds the relay + the
     * WH-console-join; otherwise the legacy Path A join stands (local/LAN). */
    /* With the host-level WH the session IS the connection - there is
     * no radio to relay - so every join is the TCP one, then the game's own
     * browse-and-connect runs against it. */
    if (lsw2_relay_auto_enabled() && !lsw2_vwh_enabled())
        return join_relay(entry.address.c_str(), entry.port);
    if (!join_address(entry.address.c_str(), entry.port)) return false;
    if (lsw2_vwh_enabled()) lsw2_wh_console_join();
    return true;
}

bool Lsw2ForkSession::join_relay(const char* address, int port) {
    if (!address || !*address) return false;

    /* NO Path A. The real WH (over the relay) carries the whole session - lobby,
     * roster and match - the way relay_match proved. Opening the
     * fork's Lsw2TcpTransport here would make the host's take_joined() broadcast
     * seed/settings/roster, which fights the real WH and bounces the game state
     * to -1 (measured 2026-09-22). So we start the relay and drive the cartridge's
     * own wireless join, and leave transport_ untouched. */
    lsw2_wh_set_role(false);
    state_ = Lsw2SessionState::Joining;
    /* The joiner is WH AID 1 in a 2-player game (the host is 0); this is also the
     * runner instance index the relay's inject/catch ports are keyed on. */
    local_station_ = 1;

    if (lsw2_relay_auto_enabled()) {
        const int base = lsw2_relay_wireless_base();
        const uint16_t relay_port = uint16_t(port + 1);
        if (lsw2_relay_session_start_explicit(/*is_host=*/false, relay_port, address,
                                              base, /*local=*/1, /*remote=*/0))
            std::fprintf(stderr,
                         "[fork] relay: dialing %s:%u (wireless base %d, AID 1, "
                         "remote 0) - real WH over relay, NO Path A\n",
                         address, relay_port, base);
        else
            std::fprintf(stderr, "[fork] relay: could not dial %s:%u\n",
                         address, relay_port);
    }

    /* The proven association: scan for the host's beacon (arriving over the relay)
     * and connect the cartridge's own way. The relay join FORCES the real-WH browse
     * route (a remote host is only reachable by scanning over the relay), so the
     * player never needs LSW2_JOIN_WM. */
    was_host_ = false;
    state_ = Lsw2SessionState::Joined;
    lsw2_wh_set_force_browse(true);
    lsw2_wh_console_join();
    std::fprintf(stderr, "[fork] join_relay: real-WH join to %s:%d armed "
                 "(AID 1, relay up)\n", address, port);
    return true;
}

void Lsw2ForkSession::tick() {
    if (!transport_) return;
    transport_->service();
    /* The host-level WH's control records, every frame: a joiner's START has to
     * be taken BEFORE the game will step - the game only steps once the joiner
     * is DATASHARING, and it becomes DATASHARING on START. */
    if (vwh_) vwh_->service();

    for (int joined = transport_->take_joined(); joined >= 0;
         joined = transport_->take_joined()) {
        /* The name arrives with the handshake in a later step; until then a
         * station number is truthful where an invented name would not be. */
        /* A PLACEHOLDER UNTIL THE NAME ARRIVES. The socket is accepted before
         * the peer has said anything, and the seat has to exist for the
         * lockstep to wait on it. The name record renames it a frame or two
         * later, and the join popup waits for that rather than announcing
         * "Player 2 has joined the game!". */
        char name[24];
        std::snprintf(name, sizeof name, "Player %d", joined + 1);
        if (lockstep_) lockstep_->seat(joined, name, false);
        if (vwh_) vwh_->admit(joined);
        peer_joined(name, joined, /*announce=*/false);

        /* THE HOST'S EXTRAS reach each client as it joins. Broadcast rather than unicast because the transport has no
         * per-peer send and the cost is four small records; a client that
         * already had them simply replaces them with the same values. */
        broadcast_settings();
        /* A SEED FOR THIS LOBBY, chosen once and re-sent to every joiner.
         *
         * Without it the four simulations diverge on the first random
         * draw, guaranteed rather than by chance. Chosen when the first peer
         * arrives rather than at start_lobby(), so it is fresh per lobby but
         * stable across joins - a seed that changed per joiner would put the
         * earlier players on a different sequence from the later ones.
         *
         * The VALUE does not need to be unpredictable. It needs to be the same
         * everywhere, which is the whole requirement. */
        if (match_seed_ == 0) {
            const uint32_t fresh =
                uint32_t(listen_port_) * 2654435761u ^
                uint32_t(console_.peer_count + 1) * 40503u ^ 0x5BD1E995u;
            set_match_seed(fresh ? fresh : 1u);
        } else {
            transport_->send_seed(match_seed_);
        }

        /* AND THE WHOLE ROSTER, not just our own name.
         *
         * Names are announced once, when a peer joins. The third player to
         * arrive would otherwise be told the host's name and nothing else - the
         * second player's record was relayed before the third had a socket to
         * receive it on, and nothing ever repeated it. So the host repeats
         * every name it knows whenever somebody new appears. */
        transport_->send_name(player_name_);
        for (int i = 0; i < console_.peer_count; ++i) {
            if (console_.peer_station[i] < 0) continue;
            if (!peer_announced_[i]) continue;   /* still a placeholder */
            transport_->send_name_as(console_.peer_station[i],
                                     console_.peers[i]);
        }
    }

    for (int lost = transport_->take_lost(); lost >= 0;
         lost = transport_->take_lost()) {
        if (lockstep_) lockstep_->drop(lost);
        if (vwh_) vwh_->depart(lost);

        /* BY STATION, which is what the transport actually told us.
         *
         * This used to rebuild the placeholder name the peer was seated under
         * and search for it - which works only for as long as the seating name
         * and the lookup name are generated by the same two lines. The moment
         * the handshake supplies a real name (Phase C), the search would stop
         * matching and a dropped peer would stay in the list for ever while its
         * socket was gone. The station is the identity; the name is a label. */
        const int row = peer_row_for_station(lost);
        if (row >= 0) peer_left_at(row);

        /* THE HOST ITSELF IS GONE - it kicked this console, shut the lobby
         * down, or the link dropped. Measured 2026-09-23 (conn_console1): a
         * kicked client kept its session "live" with nobody left to commit a
         * dataset, and sat in a frozen lobby for good. Ending the session is
         * the pause menu's Disconnect without the menu: shut_down_lobby()
         * unbinds the host-level WH, the binding reports the ended session to
         * the game the way the radio would, and the game's own handling
         * puts the player back in the lobby, offline. Nothing after this may
         * touch the transport - it is gone. */
        if (!is_host() && lost == 0) {
            std::fprintf(stderr, "[fork] the host closed the connection (kicked, "
                         "or the lobby shut down) - leaving the session\n");
            shut_down_lobby();
            host_lost_ = true;
            return;
        }
    }

    /* HEARTBEAT ABOUT ONCE A SECOND, not every frame. The directory only needs
     * to know the lobby is alive and how full it is; sixty requests a second
     * would be a denial of service against our own server. */
    /* TELL THE OTHERS WHO WE ARE PLAYING AS.
     *
     * Byte 6 of every block's header. Cheap enough to set unconditionally: it
     * is a comparison and an assignment, and the alternative is remembering to
     * set it at each of the several places a character can change - the select
     * screen, the roster rule, the boot warp's remembered last-played hunter,
     * a respawn.
     *
     * This is what last pass's correction needed. The roster rule computes the
     * same answer on every machine, but only ever ASSIGNED the local player;
     * now every machine can see what the others actually became, which is both
     * the agreement check and what remote avatars will read. */
    /* REAL NAMES REPLACE THE PLACEHOLDERS.
     *
     * A peer is seated the moment its socket is accepted, before it has said
     * anything, so it is seated as "Player 2" and renamed when its name record
     * arrives a frame or two later. The join popup is deliberately raised HERE
     * rather than at accept time - "<Playername> has joined the game!" with a
     * placeholder in it would be worse than a slightly later popup with the
     * truth in it. */
    if (transport_) {
        int station = -1;
        char raw[24];
        while (transport_->take_name(&station, raw, sizeof raw)) {
            if (station == local_station_) continue;   /* our own, relayed back */

            char clean[24];
            lsw2_sanitise_name(raw, clean, sizeof clean, "Player");

            /* The joiner's browse list names the host it is connected to. */
            if (station == 0 && vwh_) lsw2_vwh_set_host_name(clean);

            /* A CLIENT IS NOT TOLD WHO WAS ALREADY THERE. The host repeats
             * every name it knows to a new joiner the moment its socket opens,
             * long before the joiner is seated - announcing those would greet
             * a joiner with "<host> has joined the game!" for a host it just
             * joined, which is what every client's log showed until now. So a
             * client raises join popups only once its own session is live. */
            const bool announce = is_host() || !vwh_ || vwh_->live();

            const int row = peer_row_for_station(station);
            if (row >= 0) {
                std::snprintf(console_.peers[row], sizeof console_.peers[0],
                              "%s", clean);
                if (lockstep_) lockstep_->seat(station, clean, false);
                if (!peer_announced_[row]) {
                    peer_announced_[row] = true;
                    if (announce) lsw2_raise_join_popup(clean);
                }
            } else if (!is_host() && station >= 0 && station < 4) {
                /* ANOTHER CLIENT, which a client keeps out of its seat table
                 * (forksession.h, station_name_). Its name is still what the
                 * popup says when it arrives, and when it leaves. */
                std::snprintf(station_name_[station], sizeof station_name_[0],
                              "%s", clean);
                if (!station_known_[station]) {
                    station_known_[station] = true;
                    if (announce) lsw2_raise_join_popup(clean);
                }
            }
        }
    }

    /* A CLIENT SEES THE OTHERS LEAVE IN THE DATASETS.
     *
     * The departure popup was the host's alone: it is raised where a socket
     * closes (take_lost above), and a client holds one socket - to the host.
     * Measured 2026-09-23 with four consoles, the fourth closed mid-match: the
     * host said "<name> has disconnected..." and the two other clients said
     * nothing. The departure popup is for everyone still playing.
     *
     * The session's membership is the same on every console at the same
     * dataset - the host drops a departed station from its next commit - so a
     * station leaving the consumed membership IS the departure, seen by every
     * client at one step. Not while the session itself is down: a client whose
     * session ended has lost the host, not the others. */
    if (!is_host() && vwh_) {
        const bool live = vwh_->live();
        const uint16_t members = live ? vwh_->members() : 0;
        const uint16_t gone = uint16_t(client_members_ & ~members);
        for (int st = 1; live && st < 4; ++st) {
            if (!((gone >> st) & 1u) || st == vwh_->aid()) continue;
            const int row = peer_row_for_station(st);
            char name[24];
            if (row >= 0)
                std::snprintf(name, sizeof name, "%s", console_.peers[row]);
            else if (station_known_[st])
                std::snprintf(name, sizeof name, "%s", station_name_[st]);
            else
                std::snprintf(name, sizeof name, "Player %d", st + 1);
            lsw2_raise_disconnect_popup(name);
            std::fprintf(stderr, "[fork] station %d left the session's datasets "
                         "(members %X -> %X)\n", st, unsigned(client_members_),
                         unsigned(members));
            /* A later player seated in the same station is a new arrival. */
            station_known_[st] = false;
            station_name_[st][0] = '\0';
        }
        client_members_ = members;
    }

    /* AND THE SHARED SEED, which travels the same way and for a harder reason:
     * without it the four simulations diverge on the first random draw. */
    if (!is_host() && transport_) {
        uint32_t seed = 0;
        if (transport_->take_seed(&seed)) {
            match_seed_ = seed;
            std::fprintf(stderr, "[fork] match seed from the host: %08X\n",
                         unsigned(seed));
        }
    }

    /* THE CLIENT ADOPTS THE HOST'S RULES.
     *
     * Until this existed, effective_policy() read the LOCAL settings, so a
     * client applied its own customization rule and the host's say over whether
     * customization is allowed stopped at the host's own machine. Same for the
     * Extras. */
    if (!is_host() && transport_) {
        uint8_t wire[kLsw2HostWireBytes] = {0};
        Lsw2HostSettings adopted = settings_;
        if (transport_->take_settings(wire, sizeof wire) &&
            lsw2_host_settings_from_wire(adopted, wire, sizeof wire)) {
            /* BORROWED, NOT KEPT: the player's own rules wait in own_settings_
             * and come back when the session ends. */
            if (!borrowing_) {
                own_settings_ = settings_;
                borrowing_ = true;
            }
            settings_ = adopted;
            std::fprintf(stderr,
                         "[fork] adopted the host's match rules "
                         "(roster %d, customization %d, extras %s)\n",
                         int(settings_.roster), int(settings_.customization),
                         settings_.any_extra() ? "on" : "off");
            if (screen_ != Lsw2ForkScreen::None) rebuild_menu();
        }
    }

    /* WHILE HOSTING, WHICH INCLUDES DURING A MATCH.
     *
     * These two tests were `state_ == Hosting`, and that is a different set:
     * Phase D moves a host to InMatch when a round starts. Nothing sets InMatch
     * yet, so the difference is invisible today - and the day it is set, the
     * host would silently stop announcing. Three things follow from that, all
     * bad and none obviously connected to a state enum:
     *
     *   - the lobby expires from the directory after its TTL (now 20 s), so
     *     joining a match in progress quietly stops working;
     *   - the LAN beacon goes quiet, so the same on the local network;
     *   - apply_directives() stops running, so an operator's Kick is never
     *     delivered - during a match, which is precisely when the behaviour
     *     that earns a kick happens.
     *
     * is_host() already spans both states, because it was written knowing
     * InMatch was coming. Using it here is the same reading.
     *
     * The beacon rate-limits itself, so this is one comparison a frame. */
    if (is_host())
        lan_.tick(player_count(), kLsw2MaxStations);

    if (is_host() && settings_.visible_master) {
        /* EVERY FIVE SECONDS (300 frames): four beats inside the directory's
         * 20 s TTL, and a fifth of the writes the hosted directory (a
         * Cloudflare Worker over D1) took at one a second. */
        if (++heartbeat_ >= 300) {
            heartbeat_ = 0;
            publish_roster();
            /* Off the frame thread: a beat to the hosted directory takes
             * 100-300 ms, and a host frozen that long stalls every joiner's
             * lockstep with it. Directives from an earlier beat's reply are
             * drained here; this beat's arrive with the next. */
            master_.heartbeat_async(player_count());
            apply_directives();
        }
    }
}

/* The host settings menu's one-character picker, which is no longer a menu.
 * The fork-drawn text list of names that stood here is replaced by the
 * cartridge's Free Play grid - see rebuild_menu's Character case and
 * freeplaypick.h. */
void Lsw2ForkSession::finish_character_pick(int character) {
    if (screen_ == Lsw2ForkScreen::Character) {
        if (character >= 0) settings_.character = character;
        screen_ = Lsw2ForkScreen::HostSettings;
        rebuild_menu();
    }
}

/* The host's match rules, onto the wire. Called when a peer joins and whenever
 * the host closes the settings menu - closing with Return confirms any changed
 * settings. */
/* Tell the other machines who this station is playing as.
 *
 * TAKES THE VALUE RATHER THAN READING IT. Reading the character means reading
 * the guest, and this file deliberately does not - that is what lets the whole
 * session run in a test with four stations and no ROM. fork.cpp's tick does the
 * read and hands it here. A first version called
 * lsw2_cheats_party_character(0) directly and broke the session test's link,
 * which is the constraint doing its job.
 *
 * Only sent when it changes: the transport puts it on every block regardless,
 * so this is about not doing the comparison sixty times a second in two places.
 */
void Lsw2ForkSession::set_in_match(bool in_match) {
    if (state_ == Lsw2SessionState::Offline) return;      /* nothing to be in */
    if (state_ == Lsw2SessionState::Joining) return;      /* not seated yet */

    if (in_match) {
        if (state_ != Lsw2SessionState::InMatch) {
            state_ = Lsw2SessionState::InMatch;
            std::fprintf(stderr, "[fork] match started (%s)\n",
                         was_host_ ? "hosting" : "joined");
        }
        return;
    }

    if (state_ == Lsw2SessionState::InMatch) {
        /* BACK TO WHICHEVER SIDE WE WERE. was_host_ is exactly this question
         * and is already cleared on join and on shutdown, so it is the honest
         * source rather than a second flag to keep in step. */
        state_ = was_host_ ? Lsw2SessionState::Hosting
                           : Lsw2SessionState::Joined;
        std::fprintf(stderr, "[fork] match ended, back to the lobby\n");
    }
}

void Lsw2ForkSession::publish_character(int character_id) {
    if (!transport_) return;
    if (character_id == last_published_character_) return;
    last_published_character_ = character_id;
    transport_->set_local_character(character_id);
}

void Lsw2ForkSession::send_start_match(unsigned char map, unsigned char rule,
                                       unsigned short seconds) {
    if (transport_) transport_->send_start_match(map, rule, seconds);
}

bool Lsw2ForkSession::take_start_match(unsigned char* map, unsigned char* rule,
                                       unsigned short* seconds) {
    return transport_ ? transport_->take_start_match(map, rule, seconds)
                      : false;
}

void Lsw2ForkSession::send_netmsg(int station, const unsigned char* record,
                                  int len) {
    if (transport_) transport_->send_netmsg(station, record, len);
}

bool Lsw2ForkSession::take_netmsg(int* station, unsigned char* out, int* len) {
    return transport_ ? transport_->take_netmsg(station, out, len) : false;
}

int Lsw2ForkSession::peer_character(int station) const {
    return transport_ ? transport_->peer_character(station) : -1;
}

int Lsw2ForkSession::peer_slot_character(int slot) const {
    if (slot < 1 || slot > console_.peer_count) return -1;
    const int station = console_.peer_station[slot - 1];
    if (station < 0) return -1;
    return peer_character(station);
}

void Lsw2ForkSession::set_match_seed(uint32_t seed) {
    match_seed_ = seed;
    /* The host tells everyone; a client is only recording what it was told. */
    if (is_host() && transport_) transport_->send_seed(seed);
}

void Lsw2ForkSession::broadcast_settings() {
    if (!is_host() || !transport_) return;
    uint8_t wire[kLsw2HostWireBytes] = {0};
    const int n = lsw2_host_settings_to_wire(settings_, wire, sizeof wire);
    if (n > 0) transport_->broadcast_settings(wire, n);
}

void Lsw2ForkSession::publish_roster() {
    std::vector<Lsw2RosterEntry> seats;

    /* SEAT 0 IS THE HOST, and it is not in console_.peers - that list is the
     * people who joined US. An operator looking at the console window would
     * otherwise see a four-player lobby whose list held three names and wonder
     * which one was missing. No address for it: the directory takes the host's
     * from the socket the registration arrived on, which is the one it can
     * trust. */
    seats.push_back({0, player_name_, ""});

    for (int i = 0; i < console_.peer_count; ++i) {
        Lsw2RosterEntry seat;
        seat.slot = console_.peer_station[i] >= 0 ? console_.peer_station[i]
                                                  : i + 1;
        seat.name = console_.peers[i];
        if (transport_)
            seat.address = transport_->peer_address(console_.peer_station[i]);
        seats.push_back(seat);
    }
    master_.set_roster(seats);
}

void Lsw2ForkSession::apply_directives() {
    for (const Lsw2Directive& order : master_.take_directives()) {
        if (order.action != "kick") {
            /* An unknown action is a newer directory talking to an older game.
             * Say so and carry on - refusing to heartbeat would take the lobby
             * off the list over a message we merely did not understand. */
            std::fprintf(stderr, "[fork] ignoring unknown directive '%s' from "
                                 "the master server\n", order.action.c_str());
            continue;
        }

        /* BY STATION, NOT BY ROW. The directive names the seat the directory
         * was told about; rows shift every time somebody leaves. Looking up the
         * row from the station is the same guard kick_peer() already needed for
         * duplicate names. */
        const int row = peer_row_for_station(order.slot);
        if (row < 0) {
            std::fprintf(stderr, "[fork] master server asked to kick station "
                                 "%d, who is not here\n", order.slot);
            continue;
        }
        std::fprintf(stderr, "[fork] master server asked to kick %s\n",
                     console_.peers[row]);
        kick_peer(row);
    }
}

void Lsw2ForkSession::withdraw_from_master() {
    /* THE HARD-CLOSE PATH. shut_down_lobby() withdraws on the graceful exit,
     * but a player who closes the window (or the process dies) never reaches
     * it, so their lobby lingered in the directory until the TTL expired -
     * and a re-host meanwhile saw its own ghost. The same is_host() &&
     * visible_master test as the graceful path; nothing else is torn down,
     * because the process is going away regardless. */
    if (is_host() && settings_.visible_master)
        master_.unregister();
}

void Lsw2ForkSession::shut_down_lobby() {
    /* AND END THE CARTRIDGE'S OWN SESSION, which this function did not do.
     *
     * Shut Down Lobby did not gracefully return players to single player.
     * Measured why: everything below closes the
     * FORK'S TCP session and the cartridge's wireless stayed up, so the peer
     * was never told and the host was still on the air.
     *
     * ASKED FOR, NOT DONE HERE. The cartridge's own end polls WH_End in a
     * do-while, so it cannot be called from this thread; it is run from the
     * receive-pump hook, which was measured firing once a frame
     * on the guest's thread while a peer is connected. This just raises the
     * request. A client's Disconnect comes through here too, and its log line
     * says so rather than "Shut Down Lobby". */
    lsw2_wh_request_session_end(is_host() ? "Shut Down Lobby" : "left the host's game");
    close_session_host_side();

    /* AND THE CARTRIDGE'S SESSION, WHICH IS THE ONE PEERS ARE IN.
     *
     * The same Shut Down Lobby defect. Everything above closes the FORK'S TCP
     * session; a peer that joined through the cartridge's own wireless
     * system - the only way peers arrive now - was never told anything.
     *
     * The second time this exact mistake was made: the first was in the
     * party-of-one guard. Two sessions exist and the code kept addressing the
     * one that no longer carries players. ASK THE RIGHT AUTHORITY. */
    lsw2_wh_end_session();

    std::fprintf(stderr, "[fork] lobby shut down\n");
}

/* EVERYTHING shut_down_lobby() CLOSES ON THIS SIDE, and nothing that runs the
 * guest - so abandon_session() can use it on a console whose ARM9 has stopped. */
void Lsw2ForkSession::close_session_host_side() {
    /* Shut Down Lobby closes every connection and leaves the host in the lobby,
     * offline, until the console entity is used again - so the
     * host stays in the lobby map and the console still works; only the
     * session ends. */
    /* is_host(), NOT state_ == Hosting, for the same reason the heartbeat uses
     * it: a host can shut the lobby down from the pause menu
     * DURING a match, and Phase D will have them in InMatch when they do. A
     * narrower test would skip the withdrawal and leave a phantom lobby in the
     * browser for the full TTL - refusing every connection, which is
     * precisely the "broken host rather than a closed one" the note below
     * refuses to accept for the LAN beacon. */
    if (is_host() && settings_.visible_master)
        master_.unregister();

    /* STOP ANNOUNCING BEFORE ANYTHING ELSE CLOSES. A browser that hears one
     * more advertisement after the listener is gone shows a lobby that refuses
     * connections, which reads as a broken host rather than a closed one. The
     * beacon has no "goodbye" datagram - expiry covers the case where a host
     * crashes - but there is no reason to make a clean shutdown look like one. */
    lan_.stop();

    /* GIVE THE PORT BACK. A mapping left behind outlives the game and, on a
     * router with a small table, crowds out the next thing that asks. */
    if (upnp_mapped_) {
        lsw2_upnp_unmap_port(listen_port_);
        upnp_mapped_ = false;
    }

    /* Stop the wireless relay with the session and give its port back.
     * Idempotent - a no-op when no relay was started (local play / env path). */
    if (lsw2_relay_auto_enabled()) {
        lsw2_relay_session_stop();
        lsw2_upnp_unmap_port(listen_port_ + 1);
    }

    /* UNBIND THE GUEST FIRST. The data-sharing overrides read this pointer
     * every frame, and destroying the session while they still hold it is a
     * use-after-free that would present as a netcode bug. */
    lsw2_net_set_active_session(nullptr, 0);
    /* The same for the host-level WH: the guest stops answering from it before
     * it is destroyed, and a sharing console then sees the session end. */
    lsw2_vwh_set_session(nullptr);

    /* SAY HOW THE SESSION ACTUALLY RAN, once, on the way out.
     *
     * `Lsw2LockstepSession::stalls()` counted every frame the guest was made
     * to wait for a peer's input, and nothing ever called it - so the one
     * number that separates "the netcode is running" from "the netcode is
     * running and the guest is starving" was invisible for the whole match.
     * That is the reason dispatch_overrides.cpp gives for its own step
     * counters, and this is the more direct measure of the two: it is the
     * transport's own count rather than the override's.
     *
     * HERE RATHER THAN AT SHUTDOWN, because the session owns the lockstep and
     * resets it on the next line - the number is final exactly now, and a
     * player who hosts twice gets one line per match instead of a total that
     * blends them.
     *
     * Only when frames were actually stepped: a lobby that was opened and
     * closed without a match has nothing to report and should say nothing. */
    if (lockstep_ && lockstep_->frame() > 0) {
        std::fprintf(stderr,
                     "[fork] session ran %d frames with %llu stalls\n",
                     lockstep_->frame(),
                     static_cast<unsigned long long>(lockstep_->stalls()));
    }

    lockstep_.reset();
    vwh_.reset();
    transport_.reset();

    /* The roster rule's spawn counter belongs to a session. Leaving one and
     * starting another must not continue its sequence - it was declared for
     * this and never called, which made it dead code rather than a decision. */
    lsw2_roster_enforce_reset();

    match_seed_ = 0;          /* a new lobby gets a new one */
    /* AND THE COUNTDOWN GOES WITH THE LOBBY. Left running, it would carry into
     * the next lobby and start a match seconds after it opened - the same class
     * of leak `lsw2_roster_enforce_reset()` above exists to stop. */
    lobby_.reset();

    /* AND THE PLAYER'S OWN HOST SETTINGS COME BACK, if a host's were borrowed
     * for this session - see own_settings_. */
    if (borrowing_) {
        settings_ = own_settings_;
        borrowing_ = false;
        std::fprintf(stderr, "[fork] left the host's rules behind: this "
                             "player's own host settings are back (music %s, "
                             "roster %d)\n",
                     settings_.music == Lsw2HostMusic::Random ? "RANDOM"
                                                              : "DEFAULT",
                     int(settings_.roster));
    }

    state_ = Lsw2SessionState::Offline;
    was_host_ = false;
    local_station_ = 0;
    console_.peer_count = 0;
    console_.room_code[0] = '\0';
    for (int st = 0; st < 4; ++st) {
        station_known_[st] = false;
        station_name_[st][0] = '\0';
    }
    client_members_ = 0;
}

void Lsw2ForkSession::abandon_session(const char* why) {
    /* A CONSOLE WHOSE GAME HAS STOPPED LEAVES ITS SESSION. A crash here halts the ARM9 for good (nds_halt: the scheduler
     * marks the slot halted until a reset) while the window, this tick and its
     * sockets carry on - so a crashed host went on heartbeating the directory
     * and was listed for as long as its window stayed open, and its peers
     * waited on a connection that would never send again. Closing the sockets
     * is what tells the peers at once (their transport reports the station
     * lost); a host also withdraws its listing. Nothing here touches the guest:
     * the cartridge's own session end (lsw2_wh_end_session) runs lifted guest
     * code, which must not run on a halted CPU. */
    std::fprintf(stderr, "[fork] this console's game has stopped (%s) - leaving "
                         "the session: sockets closed%s\n", why ? why : "halted",
                 is_host() ? ", lobby withdrawn from the directory" : "");
    close_session_host_side();
}

bool Lsw2ForkSession::test_activate_row(int row) {
    /* See the header for why a test-only driver exists at all. */
    if (screen_ == Lsw2ForkScreen::None) return false;
    if (row < 0 || row >= menu_.row_count()) return false;

    /* SAY WHICH SCREEN THE ROW LANDED ON, and what it was labelled.
     *
     * A run of this driver picked "0,10,0" for the host-and-start route and
     * reported
     * all three as "accepted", but the third never reached Start Game - and
     * there was no way to tell which screen had eaten it, because "row 0" means
     * a different thing on every screen. Reporting the row NUMBER without the
     * screen it belongs to is the same mistake as reading an offset without
     * checking the object's identity first. */
    const char* where = "?";
    switch (screen_) {
    case Lsw2ForkScreen::ArenaMenu:    where = "ArenaMenu";    break;
    case Lsw2ForkScreen::Console:      where = "Console";      break;
    case Lsw2ForkScreen::Extras:       where = "Extras";       break;
    case Lsw2ForkScreen::Kick:         where = "Kick";         break;
    case Lsw2ForkScreen::HostSettings: where = "HostSettings"; break;
    case Lsw2ForkScreen::JoinCode:     where = "JoinCode";     break;
    case Lsw2ForkScreen::MasterList:   where = "MasterList";   break;
    case Lsw2ForkScreen::Character:    where = "Character";    break;
    default: break;
    }
    std::fprintf(stderr, "[fork] test_activate_row: screen %s row %d \"%s\"\n",
                 where, row, menu_.row(row).label ? menu_.row(row).label : "");

    menu_.set_cursor(row);
    on_menu_event(Lsw2MenuEvent::Activated);
    return true;
}

void Lsw2ForkSession::on_menu_event(Lsw2MenuEvent event) {
    if (screen_ == Lsw2ForkScreen::None) return;

    if (event == Lsw2MenuEvent::Cancelled) {
        /* B backs out one level rather than closing everything: a submenu
         * returns to the menu that opened it. */
        switch (screen_) {
        case Lsw2ForkScreen::Extras:
        case Lsw2ForkScreen::Kick:
            lsw2_extras_menu_sync(menu_, settings_);
            screen_ = (screen_ == Lsw2ForkScreen::Extras)
                          ? Lsw2ForkScreen::HostSettings
                          : Lsw2ForkScreen::Console;
            rebuild_menu();
            return;
        case Lsw2ForkScreen::MasterList:
        case Lsw2ForkScreen::JoinCode:
            screen_ = Lsw2ForkScreen::ArenaMenu;
            parent_ = Lsw2ForkScreen::None;
            rebuild_menu();
            return;
        case Lsw2ForkScreen::HostSettings:
            /* Backing out of the console's settings confirms any changed
             * settings. A lobby that is already running has clients holding the
             * OLD rules until they are told, so leaving this screen is when
             * they are told. Harmless offline: broadcast_settings() is a no-op
             * without a transport. */
            broadcast_settings();

            /* Back to whatever opened it, or out if nothing did. */
            if (parent_ != Lsw2ForkScreen::None) {
                screen_ = parent_;
                parent_ = Lsw2ForkScreen::None;
                rebuild_menu();
                return;
            }
            close_screen();
            return;
        default:
            close_screen();
            return;
        }
    }

    const int row = menu_.cursor();

    if (screen_ == Lsw2ForkScreen::ArenaMenu) {
        if (event != Lsw2MenuEvent::Activated) return;
        switch (row) {
        case kArenaHost:
            open_host_settings();
            parent_ = Lsw2ForkScreen::ArenaMenu;
            break;
        case kArenaJoin:   open_join_by_code();  break;
        case kArenaMaster: open_master_list();   break;
        default: break;
        }
        return;
    }

    if (screen_ == Lsw2ForkScreen::MasterList) {
        if (event != Lsw2MenuEvent::Activated) return;
        const int lobby = lsw2_master_list_pick(list_, row);
        if (lobby < 0) {
            /* Return, or the note row - which is not selectable, so reaching
             * here means Return. */
            screen_ = Lsw2ForkScreen::ArenaMenu;
            parent_ = Lsw2ForkScreen::None;
            rebuild_menu();
            return;
        }
        const Lsw2MasterRow& pick = list_.rows[size_t(lobby)];
        /* DIAL THE ADDRESS THE ROW NAMES. A directory row is a
         * SPECIFIC host at pick.address:pick.port - unlike Join-by-code, which
         * has no address until it resolves one - so the join has to carry that
         * address, and only join_address does. This is the exact call the
         * verified two-window join uses (LSW2_TEST_JOIN_WIRELESS -> join_address),
         * and join_address sets the wireless JOIN role itself, so the cartridge scans for and connects to the host over the
         * local radio bridge just as that test showed: host and joiner reach the
         * lobby with each other visible.
         *
         * WHY NOT lsw2_wh_console_join() FIRST (the regression this replaces):
         * that opens the cartridge's stock "Browsing Wireless Games" list - right
         * for Join-by-code, which PICKS a beacon there, but wrong for a row that
         * already names its host - and, fatally, it RETURNS true before
         * join_address runs, so the picked address was never dialed. The player
         * then saw the browse list find nothing (this host was never put on the
         * radio), a stray roster in that stock list, or nothing happen at all -
         * the three symptoms seen. Removing it restores the address dial;
         * role-setting is unchanged because join_address still does it.
         *
         * BUT DIAL AND THEN BRING THE CARTRIDGE RADIO UP. Measured: with
         * join_address ALONE the joiner's own WH session never
         * leaves WH_SYSSTATE_STOP - the fork TCP link connects and the master
         * records the join, but the cartridge never scans/connects, so there is
         * no real DataSharing, the host's "player count 2" is the fork's faked
         * viewmask, and the game's own co-op reload has no peer to place. So call
         * lsw2_wh_console_join() AFTER join_address (not before - that was the
         * early-return bug): join_address dials the picked host and sets the JOIN
         * role, then console_join brings the joiner's cartridge WH up to scan for
         * and connect to that host's beacon, which is the real connection the
         * game's own flow needs. join_address first means the address is always
         * dialed; console_join is inert when real-WH is off. */
        if (join_address(pick.address.c_str(), pick.port)) {
            lsw2_wh_console_join();
            close_screen();
        }
        return;
    }

    if (screen_ == Lsw2ForkScreen::JoinCode) {
        /* Sync on every event, not only on Activated: the four character rows
         * change on left and right, and the code has to be current before
         * Connect reads it. */
        lsw2_join_menu_sync(menu_, join_, direct_host_, direct_port_);
        if (event != Lsw2MenuEvent::Activated) return;

        /* THE CARTRIDGE'S OWN WAY IN, WHEN THE REAL RADIO IS UP.
         *
         * With the wireless session in use, "Connect" here should put the
         * player in front of the CARTRIDGE'S browse list - where the host is a
         * real beacon, and picking it raises the game's own "is trying to
         * connect ... Yes/No" prompt - rather than the fork's master-server
         * join. That is connecting the way the base game's bounty hunter setup
         * does, and it happens because the player
         * walked to the console and chose it, not because the fork aimed this
         * console at a screen during boot.
         *
         * Inert everywhere else: lsw2_wh_console_join returns false when the
         * real-WH route is not running, and the fork's own join below then
         * behaves exactly as it always has. */
        if ((row == kJoinConnect || row == kJoinDirect) &&
            lsw2_wh_console_join()) {
            close_screen();
            return;
        }

        switch (row) {
        case kJoinConnect:
            if (join_code(join_.code)) close_screen();
            break;
        case kJoinDirect:
            /* "not set" is a row that says why it will not work rather than a
             * button that quietly does nothing. */
            if (direct_host_[0] && join_address(direct_host_, direct_port_))
                close_screen();
            break;
        case kJoinReturn:
            screen_ = Lsw2ForkScreen::ArenaMenu;
            parent_ = Lsw2ForkScreen::None;
            rebuild_menu();
            break;
        default:
            break;   /* a character row - left and right already did the work */
        }
        return;
    }

    if (screen_ == Lsw2ForkScreen::HostSettings) {
        lsw2_host_menu_sync(menu_, settings_);
        if (event != Lsw2MenuEvent::Activated) return;
        if (row == kHostExtras) {
            screen_ = Lsw2ForkScreen::Extras;
            rebuild_menu();
        } else if (row == kHostCharacter) {
            /* A on the Character row brings up the Free Play menu, where the
             * host browses for and picks the single character to use - the
             * cartridge's own grid, on the touch screen (freeplaypick.h). */
            screen_ = Lsw2ForkScreen::Character;
            rebuild_menu();
        } else if (row == kHostStart && is_host()) {
            /* "UPDATE LOBBY" - the row's label while hosting (rebuild_menu).
             * It used to run start_lobby() here too, which builds a new
             * transport under any seated players and registers the lobby
             * again (a new room code): a master server log showed two
             * "started a new bounty game" from one host 29 s apart. */
            update_lobby();
            close_screen();
        } else if (row == kHostStart) {
            start_lobby();
            /* THE BEACON STARTS HERE, NOT AT START GAME.
             *
             * Measured, and it explains a run that looked like two unrelated
             * faults: the host's Start Game used to bring the cartridge's
             * session up AND open the Game Settings chooser in the same breath,
             * so by the time the other console went looking there was a host
             * sitting in a chooser rather than a host advertising a game. The
             * joiner's browse list opened, found nothing to list, and closed
             * itself - which read as "the joiner cannot reach the list" and was
             * really "there is nothing on the air".
             *
             * Opening the lobby is what should put this console on the air, and
             * it is also when the lobby is meant to exist: players gather
             * in it, and the host starts the match afterwards. Splitting the
             * two restores that order. */
            lsw2_wh_console_open_lobby();
            close_screen();
        }
        return;
    }

    /* The Character screen has no menu, so no menu event reaches here from it:
     * finish_character_pick ends it. */

    if (screen_ == Lsw2ForkScreen::Extras) {
        lsw2_extras_menu_sync(menu_, settings_);
        /* The last row is Return. */
        if (event == Lsw2MenuEvent::Activated && row == menu_.row_count() - 1) {
            screen_ = Lsw2ForkScreen::HostSettings;
            rebuild_menu();
        }
        return;
    }

    if (screen_ == Lsw2ForkScreen::Console) {
        lsw2_console_menu_sync(menu_, console_);
        if (event != Lsw2MenuEvent::Activated) return;
        switch (row) {
        case kConsoleStart:
            /* With the lobby timer off, the host starts the match manually by
             * the Start Game option at the console entity - the same round
             * start the lobby timer fires, from the other of the two ways in.
             * The session cannot call it directly: netmsg.cpp reads guest
             * memory and this file is deliberately guest-free, the same split
             * rosterenforce.cpp and lobbytimer.cpp make. So it asks. */
            start_requested_ = true;
            close_screen();
            return;
        case kConsoleSettings:
            screen_ = Lsw2ForkScreen::HostSettings;
            parent_ = Lsw2ForkScreen::Console;
            rebuild_menu();
            break;
        case kConsoleKick:
            screen_ = Lsw2ForkScreen::Kick;
            rebuild_menu();
            break;
        case kConsoleShutDown:
            shut_down_lobby();
            close_screen();
            break;
        case kConsoleReturn:
            /* Return confirms any changed settings - the settings struct has
             * been kept current by sync() all along, so closing IS the
             * confirmation. */
            close_screen();
            break;
        default:
            break;
        }
        return;
    }

    if (screen_ == Lsw2ForkScreen::Kick) {
        if (event != Lsw2MenuEvent::Activated) return;
        const int peer = lsw2_kick_menu_peer(console_, row);
        if (peer < 0) {
            screen_ = Lsw2ForkScreen::Console;
            rebuild_menu();
            return;
        }
        if (!kick_peer(peer))
            std::fprintf(stderr,
                         "[fork] cannot kick %s - no station recorded\n",
                         console_.peers[peer]);
        rebuild_menu();
    }
}

void Lsw2ForkSession::peer_joined(const char* name, int station, bool announce) {
    if (!name || console_.peer_count >= 3) return;
    console_.peer_station[console_.peer_count] = station;
    peer_announced_[console_.peer_count] = announce;
    /* SANITISED AT THE FUNNEL. This is the one place a peer's name enters the
     * fork's UI - the kick list draws it, and the join popup will - so it is
     * the one place that has to be sure of it. Names do not come off the wire
     * yet; putting this in before they do is the only order that works. */
    lsw2_sanitise_name(name, console_.peers[console_.peer_count],
                       sizeof console_.peers[0], "Player");
    ++console_.peer_count;

    /* When a player joins, a popup reads "<Playername> has joined the game!" -
     * raised with the SANITISED name,
     * which is the whole reason that runs before this.
     *
     * DEFERRED WHEN THE NAME IS A PLACEHOLDER. A peer is seated the instant its
     * socket is accepted, which is before it has told us anything; announcing
     * then would put "Player 2 has joined the game!" on everyone's screen. The
     * tick raises it when the real name arrives instead. Callers that already
     * have a name - the tests, and any future path that seats a known peer -
     * pass announce = true and get the popup here as before. */
    if (announce)
        lsw2_raise_join_popup(console_.peers[console_.peer_count - 1]);
}

bool Lsw2ForkSession::kick_peer(int index) {
    if (index < 0 || index >= console_.peer_count) return false;

    const int station = console_.peer_station[index];
    if (station < 0) return false;

    /* THE ORDER MATTERS. Drop the seat before closing the socket: the lockstep
     * refuses to advance until every SEATED station has sent its block, so a
     * socket closed while the seat remained would stall everyone else waiting
     * for a peer that can no longer answer. */
    if (lockstep_) lockstep_->drop(station);
    if (transport_) transport_->disconnect(station);

    std::fprintf(stderr, "[fork] kicked %s (station %d)\n",
                 console_.peers[index], station);

    /* BY INDEX, NOT BY NAME. peer_left() searches, and names are not unique -
     * the sanitiser falls back to "Player" for anything unprintable, so two
     * peers can hold the same string. Searching would then strike an arbitrary
     * one of them off the list while a different station was disconnected. The
     * row is already known here; use it.
     *
     * It also raises the disconnect popup, which is what a kicked player's
     * departure should look like to everyone still in the lobby - that message
     * is shown on any loss, and being kicked is one. */
    peer_left_at(index);
    return true;
}

int Lsw2ForkSession::peer_row_for_station(int station) const {
    if (station < 0) return -1;
    for (int i = 0; i < console_.peer_count; ++i)
        if (console_.peer_station[i] == station) return i;
    return -1;
}

void Lsw2ForkSession::peer_left(const char* name) {
    if (!name) return;
    for (int i = 0; i < console_.peer_count; ++i) {
        if (std::strcmp(console_.peers[i], name) != 0) continue;
        peer_left_at(i);
        return;
    }
}

void Lsw2ForkSession::peer_left_at(int index) {
    if (index < 0 || index >= console_.peer_count) return;

    /* COPY THE NAME BEFORE THE SHIFT. The popup wants the departing player's
     * name and the shift is about to overwrite that row with the next one's. */
    char name[sizeof console_.peers[0]];
    std::snprintf(name, sizeof name, "%s", console_.peers[index]);
    const bool was_player = peer_announced_[index];
    const int station = console_.peer_station[index];

    /* Close the gap so the kick list has no holes in it - a list with a blank
     * row invites pressing A on nothing. The stations travel with the names,
     * or the next kick disconnects whoever used to be in this row. */
    for (int k = index; k + 1 < console_.peer_count; ++k) {
        std::memcpy(console_.peers[k], console_.peers[k + 1],
                    sizeof console_.peers[0]);
        console_.peer_station[k] = console_.peer_station[k + 1];
        /* THE ANNOUNCE FLAG TRAVELS WITH THE ROW. Without this, removing a peer
         * would leave the next one wearing its predecessor's "already
         * announced" - and that peer's join popup would never appear. */
        peer_announced_[k] = peer_announced_[k + 1];
    }
    --console_.peer_count;
    console_.peers[console_.peer_count][0] = '\0';
    console_.peer_station[console_.peer_count] = -1;

    /* A popup message in the style of the in-game time-limit ones reads
     * "<Playername> has disconnected...".
     *
     * ONLY FOR A PEER THAT SAID WHO IT WAS. Without this the host saw
     * "Player 2 has disconnected..." whenever someone viewed or joined master
     * server lobbies. A socket is seated the moment it is accepted, and the
     * lobby browser's ping (lsw2_ping.h) is a TCP connect that hangs up at once
     * - to every listed host on every browse, and once more before a join. That
     * connection never sends a name, so it never got a join popup; its leaving
     * is not a player leaving either. */
    if (was_player) {
        lsw2_raise_disconnect_popup(name);
    } else {
        std::fprintf(stderr,
                     "[fork] station %d closed before it sent a name (a lobby "
                     "browser's ping, or a join that gave up) - no popup\n",
                     station);
    }
}
