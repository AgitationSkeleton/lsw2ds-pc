/* The session's menu flow, asserted against the specified sequence.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_forksession.cpp \
 *       port/frontend/forksession.cpp port/frontend/hostsettings.cpp \
 *       port/frontend/consolemenu.cpp port/frontend/gamemenu.cpp \
 *       port/frontend/menuhost.cpp -o test_forksession && ./test_forksession
 */

#include "forksession.h"
#include "menuhost.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void cursor_to(Lsw2Menu& m, int target) {
    for (int i = 0; i < kLsw2MenuMaxRows * 3 && m.cursor() != target; ++i)
        m.step(kLsw2KeyDown);
}

/* Activate the row at `target` and hand the event to the session. */
void activate(Lsw2ForkSession& s, int target) {
    cursor_to(*s.menu(), target);
    s.on_menu_event(s.menu()->step(kLsw2KeyA));
}

}  // namespace

int main() {
    std::printf("fork session\n");

    Lsw2ForkSession s;
    check(s.state() == Lsw2SessionState::Offline, "starts offline");
    check(s.menu() == nullptr, "with no menu open");

    /* --- which directory kind this build advertises ----------------------- */
    /* The baseline build offers co-op over the same master-server directory
     * the fork uses. One static session object serves both builds, so
     * the label cannot be decided in the constructor - it was, and a baseline
     * lobby would have advertised itself as an arena and been joined by
     * players expecting one. */
    check(std::strcmp(s.kind(), "bounty") == 0,
          "defaults to bounty - a fork that never announced is still right");
    s.set_coop(true);
    check(std::strcmp(s.kind(), "coop") == 0, "the baseline advertises coop");
    s.set_coop(false);
    check(std::strcmp(s.kind(), "bounty") == 0, "and the fork advertises bounty");

    /* --- the arena menu, which is what the console opens while offline ----
     *
     * The flow starts with three options, not with the settings
     * menu. Row indices matter here rather than labels: the session switches
     * on them, so a reordered menu would send "Join by IP/Room Code" to the
     * host settings and look like the wrong screen opening. */
    s.open_arena_menu();
    check(s.screen() == Lsw2ForkScreen::ArenaMenu, "the arena menu opens");
    check(s.menu() != nullptr && s.menu()->row_count() == kArenaRowCount,
          "with the three specified entries");

    activate(s, kArenaHost);
    check(s.screen() == Lsw2ForkScreen::HostSettings,
          "Host Internet Game leads to the settings menu");

    /* B FROM THERE GOES BACK, not out. Reached from the arena menu, so backing
     * out of the settings returns to the choice that opened them - which is
     * the difference between changing your mind and losing the screen. */
    s.on_menu_event(s.menu()->step(kLsw2KeyB));
    check(s.screen() == Lsw2ForkScreen::ArenaMenu,
          "B returns to the arena menu rather than closing");

    /* Opened directly, with nothing behind it, B still closes. */
    s.open_host_settings();
    s.on_menu_event(s.menu()->step(kLsw2KeyB));
    check(s.screen() == Lsw2ForkScreen::None,
          "but B closes when nothing opened it");

    /* --- "Host Internet Game" opens the settings menu --------------------- */
    s.open_host_settings();
    check(s.screen() == Lsw2ForkScreen::HostSettings, "Host opens the settings");
    check(s.menu() != nullptr, "and a menu is open");
    check(lsw2_menuhost_current() == s.menu(),
          "menuhost points at it, so input and drawing agree");

    /* --- A on Extras opens the submenu, B returns to the settings --------- */
    activate(s, kHostExtras);
    check(s.screen() == Lsw2ForkScreen::Extras, "A on Extras opens the submenu");
    s.on_menu_event(s.menu()->step(kLsw2KeyB));
    check(s.screen() == Lsw2ForkScreen::HostSettings,
          "B backs out one level, not all the way");
    check(s.menu() != nullptr, "so a menu is still open");

    /* Toggling an Extra in the submenu must survive the trip back. */
    activate(s, kHostExtras);
    cursor_to(*s.menu(), 0);
    s.on_menu_event(s.menu()->step(kLsw2KeyRight));
    s.on_menu_event(s.menu()->step(kLsw2KeyB));
    check(s.settings().any_extra(), "an Extra toggled in the submenu sticks");
    check(std::strcmp(s.menu()->value_of(kHostExtras), "ON") == 0,
          "and the Extras row now reads ON");

    /* --- "Start Lobby" ---------------------------------------------------- */
    activate(s, kHostStart);
    check(s.state() == Lsw2SessionState::Hosting, "Start Lobby starts hosting");
    check(s.menu() == nullptr, "and closes the menu");
    check(s.is_host(), "the player is the host");
    check(s.player_count() == 1, "with one player so far");

    /* --- peers ------------------------------------------------------------ */
    s.peer_joined("Greedo2", 1);
    s.peer_joined("Bossk", 2);
    check(s.player_count() == 3, "two joiners make three players");

    /* --- the console entity ----------------------------------------------- */
    check(s.open_console(), "the host can open the console");
    check(s.screen() == Lsw2ForkScreen::Console, "and it shows the console menu");

    /* "Change Settings" reopens the same state, not a fresh one. */
    activate(s, kConsoleSettings);
    check(s.screen() == Lsw2ForkScreen::HostSettings, "Change Settings reopens");
    check(s.settings().any_extra(),
          "with the Extra from earlier still set - the struct is the truth");

    /* --- Kick Player ------------------------------------------------------- */
    s.open_console();
    activate(s, kConsoleKick);
    check(s.screen() == Lsw2ForkScreen::Kick, "Kick Player opens the list");
    check(s.menu()->row_count() == 3, "two peers plus Return");
    cursor_to(*s.menu(), 0);
    s.on_menu_event(s.menu()->step(kLsw2KeyA));
    check(s.player_count() == 2, "kicking one leaves two players");
    check(std::strcmp(s.console().peers[0], "Bossk") == 0,
          "and the list closes up rather than leaving a hole");
    /* THE STATIONS MOVE WITH THE NAMES. Kicking row 0 must leave row 0 holding
     * Bossk's station, not Greedo2's - otherwise the next kick disconnects
     * whoever used to be there, which is the worst possible off-by-one. */
    check(s.console().peer_station[0] == 2,
          "and the kicked row's station goes with it");
    check(s.console().peer_station[s.console().peer_count] == -1,
          "with the vacated slot cleared rather than left stale");

    /* TWO PEERS, ONE NAME. The sanitiser falls back to "Player" for anything
     * unprintable, so this is reachable rather than hypothetical - and removing
     * by name would strike an arbitrary one of them off while a different
     * station was disconnected. */
    s.peer_joined("Player", 1);
    s.peer_joined("Player", 3);
    check(s.console().peer_count == 3, "three peers, two sharing a name");
    s.peer_left_at(1);
    check(s.console().peer_count == 2, "removing row 1 removes exactly one");
    check(s.console().peer_station[1] == 3,
          "and it is the RIGHT one - row 1 now holds the second Player");

    /* THE STATION LOOKUP, which is what a dropped connection uses. Two peers
     * share a name here, so a name search cannot tell them apart and only the
     * station can. */
    check(s.peer_row_for_station(3) == 1, "a station finds its own row");
    check(s.peer_row_for_station(99) == -1, "an unknown station finds none");
    check(s.peer_row_for_station(-1) == -1, "and so does an unseated one");

    /* --- Shut Down Lobby --------------------------------------------------- */
    s.open_console();
    activate(s, kConsoleShutDown);
    check(s.state() == Lsw2SessionState::Offline, "Shut Down returns to offline");
    check(!s.is_host(), "and the player is no longer host");
    check(s.player_count() == 1, "peers are dropped");

    /* "until the host again interacts with the console entity" - the console
     * must still work while offline, or the lobby could never be restarted. */
    check(s.open_console(), "the console still opens when offline");

    /* --- an empty kick list does not open --------------------------------- */
    activate(s, kConsoleKick);
    check(s.screen() == Lsw2ForkScreen::Console,
          "Kick with nobody connected stays on the console");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (!g_failures) std::printf("all checks passed\n");
    return g_failures ? 1 : 0;
}
