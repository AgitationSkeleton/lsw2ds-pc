/* The fork's multiplayer session: what the fork is doing about networking right now.
 *
 * WHAT THIS OWNS. One state machine, the settings the host has chosen, and the
 * decision of which menu is open. It does NOT own sockets - the transport
 * (port/net/lsw2_tcp.h), the lockstep session and the master client already
 * exist and are tested. This is what drives them and what the menus talk to.
 *
 * WHY SEPARATE FROM THE MENUS. The hosting flow crosses several screens - the
 * arena menu opens host settings, "Start Lobby" advertises and puts the host in
 * the lobby, the console entity reopens settings mid-session, "Shut Down Lobby"
 * returns to offline - and none of those transitions belong to a menu. A menu
 * that also knew about lobbies would have to know about all of them.
 *
 * THE STATES, from the design of the flow:
 *
 *   Offline    the fork's normal lobby, nothing advertised
 *   Hosting    a lobby is up and advertised; players may join
 *   Joining    a connection is being made to someone else's lobby
 *   Joined     in someone else's lobby as a client
 *   InMatch    a round is running (Phase D drives this)
 *
 * "Shut Down Lobby" leaves the host in the main lobby, offline, until the
 * console entity is used again - so Offline is reachable from
 * Hosting and the console entity works in both.
 */

#ifndef LSW2_FORKSESSION_H
#define LSW2_FORKSESSION_H

#include "characterpick.h"
#include "consolemenu.h"
#include "joinmenu.h"
#include "masterlist.h"
#include "hostsettings.h"
#include "lobbytimer.h"

#include "../net/lsw2_lan.h"
#include "../net/lsw2_lockstep.h"
#include "../net/lsw2_master.h"
#include "../net/lsw2_tcp.h"
#include "../net/lsw2_wsrelay.h"
#include "../net/lsw2_vwh.h"

#include <memory>

enum class Lsw2SessionState { Offline, Hosting, Joining, Joined, InMatch };

/* Which of the fork's menus is showing. Kept here rather than in menuhost so
 * that "what should be open given the session state" is one decision. */
enum class Lsw2ForkScreen {
    None,
    ArenaMenu,      /* the three entries the fork replaces         */
    JoinCode,       /* "Join by IP/Room Code" -> the code picker    */
    MasterList,     /* "View Master Server" -> the lobby browser    */
    HostSettings,   /* "Host Internet Game" -> the ten-row menu   */
    Extras,         /* A on the Extras row                        */
    Character,      /* A on the Character row - the host's pseudo-freeplay pick */
    Console,        /* the lobby's console entity                 */
    Kick,           /* "Kick Player" -> the peer list             */
};

class Lsw2ForkSession {
public:
    /* THE KIND IS A PROPERTY OF THE BUILD, NOT A SETTING, so it is declared
     * here and not in configure().
     *
     * It was in configure() for two ticks, and configure() is called from the
     * imgui Multiplayer tab's draw - which means it only runs while that tab is
     * OPEN. A player who never opened it left the client with no kind, so its
     * browser asked for every lobby rather than arena ones. That reads as
     * correct today only because nothing else advertises yet; the master
     * server's design puts co-op lobbies in the same directory, and they would
     * have appeared in the fork's arena list. */
    Lsw2ForkSession();

    Lsw2SessionState state() const { return state_; }
    Lsw2ForkScreen screen() const { return screen_; }

    Lsw2HostSettings& settings() { return settings_; }
    const Lsw2HostSettings& settings() const { return settings_; }
    Lsw2ConsoleState& console() { return console_; }
    const Lsw2JoinState& join_state() const { return join_; }
    const Lsw2ConsoleState& console() const { return console_; }

    /* The menu currently open, or null. Owned here; menuhost points at it. */
    Lsw2Menu* menu();

    /* THE ARENA MENU ITSELF - what the console entity opens while offline.
     *
     * The hosting flow starts here: three options, of which "Host Internet
     * Game" leads to the ten-row settings menu and "Start Lobby". Opening the
     * settings menu directly would skip the step where the player chooses to
     * host rather than to join. */
    void open_arena_menu();

    /* THE ARENA MENU'S THREE ENTRIES, which the fork relabels in place.
     * Called when the player picks one. */
    void open_host_settings();
    void open_join_by_code();
    void open_master_list();

    /* JUST THE FETCH, without opening the fork's browser.
     *
     * The baseline co-op build shows the same lobbies on the CARTRIDGE's
     * screen, so it needs the directory and the LAN merged into `list_` and
     * nothing else. `lan_first` puts LAN lobbies at the top, highlighted in
     * green - the fork passes false, because its own list has LAN lobbies mixed
     * into the list. */
    void refresh_lobbies(bool lan_first);

    /* How many lobby rows the last refresh found (master server + LAN), for a
     * test to confirm discovery without reaching into list_. */
    int list_row_count() const;

    /* Whether this machine may use the lobby's console entity - the host
     * alone may. Returns false for a client, which is what makes walking
     * into it do nothing rather than opening a menu with no authority. */
    bool open_console();

    void close_screen();

    /* One frame. `event` is what the open menu just reported, so the session
     * acts on activations without the menu knowing what they mean. */
    void on_menu_event(Lsw2MenuEvent event);

    /* THE HOST'S ONE CHARACTER, CHOSEN ON THE FREE PLAY GRID (freeplaypick.h).
     * The Character screen opens no menu - the grid on the touch screen is the
     * picker - and fork.cpp ends it here with the pick, or -1 for B. Either
     * way the host settings menu comes back, its Character row showing the
     * result. Does nothing off that screen. */
    void finish_character_pick(int character);

    /* ACTIVATE ONE ROW, FOR A TEST DRIVE ONLY.
     *
     * WHY THIS EARNS ITS PLACE, on the same grounds LSW2_TEST_CONSOLE did. The
     * fork's menus are composited by the port, so `pilot shot` - which reads
     * the framework's framebuffer - photographs a blank starfield where the
     * menu is; and they are driven by the port's own input path, so the
     * framework's key injection does not reach them either. A menu that can be
     * neither seen nor pressed over the debug surface cannot be verified at
     * all, and the hosting flow - the host walking to the console
     * entity and choosing "Start Game" - lives entirely inside one.
     *
     * Returns false if no menu is open or the row does not exist, so a caller
     * that mistimed it learns rather than silently doing nothing. */
    bool test_activate_row(int row);

    /* Called by the transport layer as peers come and go. Keeps the kick list
     * and the advertised player count honest. */
    /* `announce` raises the "has joined the game!" popup. Default true so an explicit caller -
     * a test, or any path that already knows the name - behaves as before; the
     * accept path passes false and lets the tick announce once the peer's real
     * name arrives, rather than saying "Player 2 has joined the game!". */
    void peer_joined(const char* name, int station = -1, bool announce = true);
    void peer_left(const char* name);

    /* REMOVE BY ROW, which is what the kick list already knows.
     *
     * peer_left() searches by name, and names are not unique: the sanitiser
     * falls back to "Player" for anything unprintable, so two peers can hold
     * the same string. Removing the first match is then removing an arbitrary
     * one of them - and for a kick, arbitrary means disconnecting one player
     * while striking another's name off the list. */
    void peer_left_at(int index);

    /* The row holding a given station, or -1. The station is what the transport
     * reports on a loss and what the kick list needs to act on, so this is the
     * lookup both should use - a name search is a different question that
     * happens to usually agree. */
    int peer_row_for_station(int station) const;

    /* The kicked player is disconnected - the transport drop as well as the list
     * removal. Returns false when the row has no station recorded. */
    bool kick_peer(int index);
    int player_count() const { return 1 + console_.peer_count; }

    /* HOW MANY PLAYERS THE CARTRIDGE'S OWN SESSION HAS SEATED, which is not the
     * same question as player_count() and is the one the lobby timer needed.
     *
     * MEASURED. Two consoles connected through the cartridge's wireless system,
     * the host's netgame bookkeeping reported `players 2`, and the lobby
     * countdown never started - so the round never began, and the rule that the
     * timer begins once a second player joins simply did not happen.
     *
     * `player_count()` is `1 + console_.peer_count`, and `console_.peer_count`
     * is only ever incremented by the fork's OWN seat path. A peer that arrived
     * over the cartridge's wireless is invisible to it, so the count stayed 1
     * and the timer's precondition never held.
     *
     * THIS IS THE SAME WRONG AUTHORITY AN EARLIER FIX REMOVED ELSEWHERE - asking
     * the fork's TCP session about a player the cartridge admitted. It is recorded
     * as a separate field rather than folded into player_count() because both
     * numbers are real: the fork's own transport genuinely has its own peers
     * when a lobby is joined by IP or room code, and the cartridge's session
     * genuinely has its own when joined over wireless. Neither is wrong; each
     * only knows about its own arrivals.
     *
     * SET FROM fork.cpp, because forksession.cpp is deliberately guest-free -
     * the same split the clock reading and rosterenforce.cpp already make, and
     * the reason the host unit test can link this file without a ROM. */
    void set_cartridge_players(int n) { cartridge_players_ = n; }

    /* JOINED THROUGH THE CARTRIDGE, NOT THE FORK'S SOCKET.
     *
     * A peer admitted through the cartridge's own browse list never
     * goes through join_address(), so state_ stays Offline and in_session()
     * is false - and the pause row that should read "Disconnect" reads "Quit
     * Game". This marks the session joined when the cartridge has seated the
     * local player as a CHILD, so the relabel, the match HUD and the rest see a
     * real session.
     *
     * IT ONLY OWNS THE STATE IT SET. A fork-hosted or fork-joined session is
     * left exactly alone: Offline is the only state it promotes to Joined, and
     * it only demotes back a Joined state it set itself (cartridge_child_), so
     * the fork's own host/join/match states are never disturbed. Set from
     * fork.cpp for the same guest-free reason set_cartridge_players is. */
    void set_cartridge_child(bool joined) {
        if (joined) {
            if (state_ == Lsw2SessionState::Offline) {
                state_ = Lsw2SessionState::Joined;
                was_host_ = false;
                cartridge_child_ = true;
            }
        } else if (cartridge_child_) {
            cartridge_child_ = false;
            if (state_ == Lsw2SessionState::Joined) state_ = Lsw2SessionState::Offline;
        }
    }

    /* The larger of the two, which is what "how many players are here" means to
     * everything above this class. MAX rather than a sum: a single player is
     * never seated by both transports at once, but if that ever changed, adding
     * would double-count them and starting a round early is worse than starting
     * it late. */
    int players_present() const {
        const int fork_side = player_count();
        return cartridge_players_ > fork_side ? cartridge_players_ : fork_side;
    }

    /* What "Start Lobby" and "Shut Down Lobby" do, exposed so the console menu
     * and the host menu can share them. */
    void start_lobby();
    void shut_down_lobby();
    /* Leave the session without running any guest code - for a console whose
     * ARM9 has halted. See forksession.cpp. */
    void abandon_session(const char* why);
    /* True once after this console, a client, lost its host - the host's
     * station dropped and the session was ended here. fork.cpp takes it to
     * bring a console that was in the match back to the lobby. */
    bool take_host_lost() {
        const bool was = host_lost_;
        host_lost_ = false;
        return was;
    }

    /* "Update Lobby": the host changed its settings with the lobby already up.
     * The transport, the session and every seated player stay; only what the
     * settings govern changes - LAN and master visibility, and the rules the
     * peers adopt. start_lobby() again would replace the transport (dropping
     * everyone) and register a second lobby with the master server. */
    void update_lobby();

    /* Withdraw this host's lobby from the master directory at process exit.
     * The graceful "Shut Down Lobby" path already unregisters; this is the
     * hard-close path (window X / crash-adjacent shutdown) so closing the game
     * delists the lobby immediately rather than leaving a ghost for the TTL.
     * A no-op when not a visible-to-master host. */
    void withdraw_from_master();

    /* Join someone else's lobby, by address or by the master server's
     * four-character code. Both end in the same place - connected as a client -
     * so they share everything after the address is known. */
    bool join_address(const char* address, int port);
    bool join_code(const char* code);

    /* Join over the REAL WH carried by the relay, with NO Path A
     * (Lsw2TcpTransport): the fork's TCP lobby/roster/seed machinery fights the
     * real WH and bounces the game state, so internet play uses the relay + the
     * cartridge's own wireless only. Starts the relay dialing <address>:(port+1)
     * and drives the WH-console-join association. This is the relay_match recipe
     * with the peer address resolved by the master/room-code/direct-IP flow. */
    bool join_relay(const char* address, int port);

    /* Once a frame. Pumps sockets, seats new arrivals, drops departures, and
     * keeps the advertised player count current. */
    void tick();

    /* The lobby's between-match countdown, driven from tick().
     *
     * `now` is the game's own seconds counter rather than a frame count, so
     * this and the statue select's 30s timer are measured the same way and a
     * slow frame does not stretch the lobby. The session passes it in because
     * forksession.cpp is deliberately guest-free - rosterenforce.cpp does the
     * same job for the roster rules.
     *
     * Returns true on the tick the match should start. `hold` while a joiner
     * is being seated: the start waits, the timer still sees the match (see
     * lobbytimer.h's Lsw2LobbyTimerInput::hold). */
    bool lobby_tick(uint32_t now, bool hold);

    /* "Start Game" on the console entity, taken by the fork tick.
     *
     * A FLAG RATHER THAN A CALL, because starting a round is a guest write and
     * this class does not touch the guest - rosterenforce.cpp and lobbytimer.cpp
     * are split the same way. Cleared by the taker, so the request cannot fire
     * twice from one press. */
    bool take_start_request();

    /* Seconds left, or -1 when no countdown is running. For a display. */
    int lobby_remaining(uint32_t now) const;

    /* WHICH KIND OF LOBBY THIS BUILD ADVERTISES AND LISTS.
     *
     * The baseline build offers co-op over the same directory, so the two builds share this object and must not share its
     * label. Called once, from `ui_set_fork_build()`, because that is where the
     * build finally becomes known - not from a tab's draw, which a player can
     * go a whole session without opening. */
    void set_coop(bool coop);

    /* What the master server is told the lobby is doing, and its rules (JSON
     * object), on the next heartbeat - for its "game started" log. */
    void set_master_status(const std::string& state, const std::string& config) {
        master_.set_status(state, config);
    }
    const char* kind() const { return master_.kind().c_str(); }

    /* The imgui Multiplayer tab's settings, which are the only part of the
     * multiplayer setup kept out of the game. */
    void configure(const char* player_name, const char* master_host,
                   int master_port, int listen_port);

    /* The direct-connect address from the Multiplayer tab, which the in-game
     * Join screen offers as its second way in. Separate
     * from configure() because it is a different address doing a different job
     * from the directory's. */
    /* Who this station is playing as, for the other machines. Called from the
     * fork tick, which does the guest read - see the note on the definition. */
    /* Each player's customization parts, both directions. The session owns the transport; the
     * guest reads and writes live in peerparts.cpp, the same split
     * rosterenforce.cpp and lobbytimer.cpp make. */
    /* WHAT THE BASELINE'S LOBBY LIST SHOWS - the master server's lobbies on the
     * cartridge's own screen.
     *
     * The cartridge's own list reads a table the fork fills, so it
     * needs three questions answered: how many lobbies, what each is called, and
     * what joining row N means. Kept here rather than in coopjoin.cpp because
     * the answers are the session's; that file only writes them into guest
     * memory, the same split every other pair in this port makes. */
    int known_lobbies() const;
    void lobby_name(int row, char* out, int n) const;
    bool join_row(int row);

    void publish_parts(const uint16_t* parts, int count);
    bool take_parts(int* station, uint16_t* out, int count);

    void publish_character(int character_id);

    /* THE MATCH-START SIGNAL over the fork transport. The host sends
     * it when it leaves the lobby for the arena; a joiner idle at state 48 takes
     * it and knows to follow into the match, since the DataSharing lockstep the
     * character rides only steps once both are already IN the match. Forwards to
     * the transport's lobby-active control channel. */
    void send_start_match(unsigned char map, unsigned char rule,
                          unsigned short seconds);
    bool take_start_match(unsigned char* map, unsigned char* rule,
                          unsigned short* seconds);

    /* Relayed netgame message records: the fork carries the
     * cartridge's own netgame message stream, which never crosses the DS-WM
     * link. send is called with the local station and a message record; take
     * yields a received one (station, bytes, len). Forwards to the transport. */
    void send_netmsg(int station, const unsigned char* record, int len);
    bool take_netmsg(int* station, unsigned char* out, int* len);

    /* THE SESSION IS IN A MATCH, or has left one.
     *
     * `InMatch` was the one state in the whole fork machine that was read and
     * never produced - `is_host()` and the imgui status switch both handled it
     * and nothing ever put the session there. Phase D will own the ROUND; this
     * owns the plainer fact that the player is on an arena map, which is
     * observable today and is what several already-written behaviours are
     * conditioned on: the heartbeat and LAN beacon keep announcing through a
     * match (so a match already under way can still be joined), the
     * console stays host-only, and the status line stops saying "Hosting" while
     * a round is running.
     *
     * Takes the answer rather than reading it, for the same reason
     * publish_character() does: this file must not touch the guest, or the
     * session stops being testable with four stations and no ROM. */
    void set_in_match(bool in_match);

    /* What a station is playing as, or -1 when it has not said. Fed by every
     * block's header; the host's roster rules use it to agree, and remote avatars
     * will use it to know which model to draw. */
    int peer_character(int station) const;

    /* The character a connected peer should be BUILT AS for party slot `slot`
     * (1..3), or -1 when that slot has no seated peer or the peer has not yet
     * published a character. Slot 0 is the local player (handled separately).
     * The local player is party slot 0 and other players are indices 1..3
     * so slot k maps to the (k-1)-th seated peer. This is what turns
     * a joined peer from the lobby's default Greedo companion into its own
     * chosen hunter (the remote-avatar seat). */
    int peer_slot_character(int slot) const;

    /* THE SHARED RANDOM SEED for the current match, or 0 before one is agreed.
     * The game seeds MT19937 from the real-time clock, so four
     * machines never agree by accident and a lockstep match diverges on the
     * first draw. The host picks; everyone applies it on entering the arena. */
    uint32_t match_seed() const { return match_seed_; }
    void set_match_seed(uint32_t seed);

    void configure_direct(const char* host, int port);
    const char* direct_host() const { return direct_host_; }
    int direct_port() const { return direct_port_; }
    const char* player_name() const { return player_name_; }
    const char* last_error() const { return last_error_; }

    /* WHICH STATION THIS MACHINE PLAYS AS. Zero while offline or hosting -
     * which is also the AID the guest gives itself - and whatever
     * the host assigned while joined.
     *
     * The match HUD needs it: it mirrors "players 2-4", meaning everyone except
     * this machine, and a client that assumed it was station 0 would hide the
     * host's portrait and show its own. */
    int local_station() const { return local_station_; }

    bool is_host() const {
        return state_ == Lsw2SessionState::Hosting ||
               (state_ == Lsw2SessionState::InMatch && was_host_);
    }

    /* Is a session up at all - hosting, joined, or mid-match?
     *
     * The line where the HOST'S rules start overriding local preferences. A
     * client may restrict itself and never widen itself, so anything that could
     * widen has to ask this first. See lsw2_hle_fork_hunters_only(). */
    bool in_session() const {
        return state_ != Lsw2SessionState::Offline;
    }

    /* Are settings() the SESSION'S rules? A host's always are; a joiner's only
     * once it has adopted the host's (borrowing_), which is a moment after it
     * joins. */
    bool session_rules_known() const { return is_host() || borrowing_; }

    /* A MATCH RULE CHANGED OUTSIDE THE HOST MENU (the F1 menu's Gameplay
     * page): a hosting session tells its clients now, as leaving the host
     * menu does. A no-op offline or for a joiner. */
    void rules_changed() {
        if (is_host()) broadcast_settings();
    }

private:
    void rebuild_menu();

    /* Tell the directory who is in the lobby, so its console window can expand
     * a row into a player list and act on one. Called whenever the seat table
     * changes; the values ride the next heartbeat. */
    /* Send the host's match rules to every client. */
    void broadcast_settings();

public:
    /* SAY THE ROUND HAS STARTED, so every client learns where to go.
     *
     * The host warps itself and then calls this; the field rides the settings
     * payload clients already adopt, and lsw2_roundfollow_tick() on the other
     * end warps to match. Nothing else in the port crosses the wire at round
     * start - lsw2_netmsg_start_round writes only the LOCAL guest's receive
     * buffer, despite its name.
     *
     * Also what joining a match already under way needs: a joiner
     * adopts the settings on arrival and so learns a round is running. */
    void announce_round(int level) {
        settings_.round_level = level;
        broadcast_settings();
    }

private:

    /* shut_down_lobby()'s host side - sockets, listing, beacon, session state -
     * without the cartridge's session end. */
    void close_session_host_side();

    void publish_roster();

    /* Carry out whatever the directory's operator queued for us. The directory
     * cannot reach a player - only the host can - so a kick arrives here on a
     * heartbeat reply and is executed locally. */
    void apply_directives();

    Lsw2SessionState state_ = Lsw2SessionState::Offline;
    Lsw2ForkScreen screen_ = Lsw2ForkScreen::None;

    /* WHERE B GOES FROM THE HOST SETTINGS MENU, which is not one answer.
     *
     * That menu is reached two ways - from the arena menu's "Host Internet
     * Game" before a lobby exists, and from the console's "Change Settings"
     * after one does - and backing out should return to whichever opened it.
     * None means it was opened directly, in which case B closes, which is what
     * it did before this field existed. */
    Lsw2ForkScreen parent_ = Lsw2ForkScreen::None;
    Lsw2HostSettings settings_;
    /* THE PLAYER'S OWN HOST SETTINGS, set aside the first time a joined
     * session's host rules are adopted into settings_, and put back when the
     * session ends. Without it, leaving a lobby
     * left the host's rules in settings_: the player's Host Settings then
     * showed the last host's choices - RANDOM music among them - and the next
     * save wrote them into the player's own file. */
    Lsw2HostSettings own_settings_;
    bool borrowing_ = false;
    bool host_lost_ = false;   /* see take_host_lost() */
    Lsw2ConsoleState console_;
    Lsw2JoinState join_;
    Lsw2MasterListState list_;

    /* The last character published on the wire, so the transport is only told
     * when it changes rather than sixty times a second. -2 rather than -1
     * because -1 is a real answer - "no actor this frame" - and must still be
     * published once. */
    int last_published_character_ = -2;
    uint32_t match_seed_ = 0;

    /* Whether each peer row has had its join popup raised. Indexed like
     * console_.peers, and shifted with it when a row is removed. */
    bool peer_announced_[3] = {false, false, false};

    /* A CLIENT'S VIEW OF THE OTHER CLIENTS, for the join/leave popups and nothing else.
     *
     * A client's seat table holds the host alone, and it has to: its rows feed
     * player_count() and the view-mask publish, which the other clients must
     * not change. So the names the host relays for the other stations are kept
     * here, by station, along with the membership of the last dataset this
     * console consumed - which is how a client learns that someone left, since
     * no socket of its own closes when they do. */
    char station_name_[4][24] = {{0}};
    bool station_known_[4] = {false, false, false, false};
    uint16_t client_members_ = 0;
    Lsw2Menu menu_;
    bool was_host_ = false;
    int local_station_ = 0;
    bool upnp_mapped_ = false;

    /* THE TRANSPORT AND THE LOCKSTEP SESSION ARE CREATED ON DEMAND, not held
     * for the life of the process. A fork that never hosts should never have
     * opened a socket, and shutting a lobby down should leave nothing
     * listening - which is easier to guarantee by destroying them than by
     * remembering to close them. */
    std::unique_ptr<Lsw2TcpTransport> transport_;
    std::unique_ptr<Lsw2LockstepSession> lockstep_;
    /* THE HOST-LEVEL WH's session, in place of the lockstep when
     * the host-level WH is on (the fork's default, lsw2_vwh_enabled): the
     * game's own wireless manager is answered from it, so the
     * cartridge's host, join, rules, statue select and match all run over this
     * transport with no emulated radio. Handed to the guest through
     * lsw2_vwh_set_session(). */
    std::unique_ptr<Lsw2VwhSession> vwh_;
    Lsw2MasterClient master_;
    Lsw2LobbyTimer lobby_;
    /* Seated by the cartridge's own session; see set_cartridge_players(). Zero
     * until fork.cpp has told us, so players_present() falls back to the fork's
     * own count and behaves exactly as before on a build that never sets it. */
    int cartridge_players_ = 0;
    /* True only while this session's Joined state was promoted from Offline by
     * set_cartridge_child - so demoting it never clears a fork-owned state. */
    bool cartridge_child_ = false;
    bool start_requested_ = false;

    /* The host settings menu's "Visible to LAN <YES/NO>", which until now was a menu row
     * with nothing behind it. Held by value rather than by pointer because a
     * beacon that is not running owns nothing - `start()` is what opens the
     * socket and the destructor closes it. */
    Lsw2LanBeacon lan_;

    /* LAN GAME SUPPORT: the local network is scanned for games hosted on
     * it.
     *
     * The BEACON above announces; this LISTENS, and until now nothing did -
     * `Lsw2LanBrowser` was written, tested and instantiated nowhere, so a player
     * could host to their own network and no one could find them from inside the
     * game. Held by value like the beacon: a browser that is not running owns
     * nothing. */
    Lsw2LanBrowser browser_;

    char player_name_[24] = "Maclunkey";   /* the specified default */
    int listen_port_ = 28550;
    char direct_host_[128] = "";
    int direct_port_ = 28550;
    const char* last_error_ = "";
    std::string last_error_text_;   /* storage when the message is built at run time */
    int heartbeat_ = 0;

    /* THE RELAY (lsw2_wsrelay.h): online play with no port forwarding. A
     * listed host keeps relay_host_ connected, so a joiner who cannot reach
     * its address directly comes in through the master server; a joiner whose
     * direct connect fails dials the lobby's room with relay_join_ and runs
     * the same transport over a loopback socket. `join_relay_id_` is the
     * lobby the next join_address() may fall back to - set by the routes that
     * know it (a directory row, a room code), consumed by the join. */
    std::unique_ptr<Lsw2RelayHost> relay_host_;
    std::unique_ptr<Lsw2RelayJoin> relay_join_;
    std::string join_relay_id_;
    bool via_relay_ = false;
    void start_relay_host();

public:
    /* True when this console joined through the relay; for the menus. */
    bool joined_via_relay() const { return via_relay_; }
    /* True when this host is reachable through the relay. */
    bool relay_online() const { return relay_host_ && relay_host_->online(); }
};

Lsw2ForkSession& lsw2_fork_session();

#endif /* LSW2_FORKSESSION_H */
