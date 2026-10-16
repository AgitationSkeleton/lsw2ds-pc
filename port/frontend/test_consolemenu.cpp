/* The console entity's menu, asserted against its design.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_consolemenu.cpp \
 *       port/frontend/consolemenu.cpp port/frontend/gamemenu.cpp \
 *       -o test_consolemenu && ./test_consolemenu
 */

#include "consolemenu.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool has_row(const Lsw2Menu& m, const char* label) {
    for (int i = 0; i < m.row_count(); ++i)
        if (m.row(i).label && std::strcmp(m.row(i).label, label) == 0) return true;
    return false;
}

}  // namespace


/* The Bounty Hunter lobby console's arena menu. Three entries, in the design's own order, all
 * selectable - the row indices are what forksession switches on, so an extra
 * or reordered row would send "Join by IP/Room Code" to the host settings. */
void test_arena_menu() {
    Lsw2Menu m;
    lsw2_arena_menu_build(m);

    check(m.row_count() == kArenaRowCount, "arena: three rows");
    check(std::strcmp(m.row(kArenaHost).label, "Host Internet Game") == 0,
          "arena: row 0 hosts");
    check(std::strcmp(m.row(kArenaJoin).label, "Join by IP/Room Code") == 0,
          "arena: row 1 joins");
    check(std::strcmp(m.row(kArenaMaster).label, "View Master Server") == 0,
          "arena: row 2 is the master list");
    for (int i = 0; i < kArenaRowCount; ++i) {
        check(m.row(i).selectable, "arena: every row is selectable");
        check(!m.row(i).hidden, "arena: no row is hidden");
    }
}

int main() {
    test_arena_menu();
    std::printf("console entity menu\n");

    Lsw2ConsoleState s;
    Lsw2Menu m;
    lsw2_console_menu_build(m, s);
    lsw2_console_menu_sync(m, s);

    /* --- the six options the design names ---------------------------------- */
    for (const char* label : {"Start Game", "Change Settings", "Show Room Code",
                              "Kick Player", "Shut Down Lobby", "Return"}) {
        char what[80];
        std::snprintf(what, sizeof what, "\"%s\" is offered", label);
        check(has_row(m, label), what);
    }

    /* --- "which off by default" ------------------------------------------- */
    check(std::strcmp(m.value_of(kConsoleShowCode), "OFF") == 0,
          "Show Room Code is off by default");
    check(m.row(kConsoleRoomCode).hidden, "so the code row is hidden");

    /* --- with no code issued, turning it on STILL SHOWS A LINE ------------
     * The Show Room Code setting once did not display the room code on the
     * next line. A toggle that does nothing visible is the bug. ------------*/
    m.row(kConsoleShowCode).value = 1;
    lsw2_console_menu_sync(m, s);
    check(s.show_room_code, "the toggle reads through to the state");
    check(!m.row(kConsoleRoomCode).hidden,
          "and the row appears even before a code has been issued");
    check(std::strcmp(m.row(kConsoleRoomCode).label, "NO CODE YET") == 0,
          "saying plainly that there is nothing to show yet");

    /* --- with a code, it appears below its own toggle ---------------------- */
    std::strcpy(s.room_code, "K7QP");
    lsw2_console_menu_build(m, s);
    m.row(kConsoleShowCode).value = 1;
    lsw2_console_menu_sync(m, s);
    check(!m.row(kConsoleRoomCode).hidden, "with a code, the row appears");
    check(std::strcmp(m.row(kConsoleRoomCode).label, "K7QP") == 0,
          "and shows the four-character code");
    check(m.row_y(kConsoleRoomCode) > m.row_y(kConsoleShowCode),
          "below the setting that toggles it");
    check(m.row_y(kConsoleKick) > m.row_y(kConsoleRoomCode),
          "and pushes Kick Player down rather than overdrawing it");

    /* Turning it off closes the gap again. */
    const int kick_with = m.row_y(kConsoleKick);
    m.row(kConsoleShowCode).value = 0;
    lsw2_console_menu_sync(m, s);
    check(m.row_y(kConsoleKick) < kick_with, "turning it off closes the gap");

    /* --- the code row is never a cursor stop ------------------------------- */
    m.row(kConsoleShowCode).value = 1;
    lsw2_console_menu_sync(m, s);
    for (int i = 0; i < kConsoleRowCount * 2; ++i) {
        m.step(kLsw2KeyDown);
        if (m.cursor() == kConsoleRoomCode) break;
    }
    check(m.cursor() != kConsoleRoomCode,
          "the cursor never lands on the code itself");

    /* --- "a list of players besides the host currently connected" ---------- */
    Lsw2Menu kick;
    check(!lsw2_kick_menu_build(kick, s),
          "with nobody else connected there is no kick list");

    std::strcpy(s.peers[0], "Maclunkey");
    std::strcpy(s.peers[1], "Greedo2");
    s.peer_count = 2;
    check(lsw2_kick_menu_build(kick, s), "with peers there is one");
    check(kick.row_count() == 3, "two players plus Return");
    check(has_row(kick, "Maclunkey") && has_row(kick, "Greedo2"),
          "and it names them");
    check(lsw2_kick_menu_peer(s, 0) == 0 && lsw2_kick_menu_peer(s, 1) == 1,
          "rows map to peers");
    check(lsw2_kick_menu_peer(s, 2) == -1, "and Return maps to no peer");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (!g_failures) std::printf("all checks passed\n");
    return g_failures ? 1 : 0;
}
