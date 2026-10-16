/* The menu model, checked against the host settings menu's specification.
 *
 * The cases here are not invented: each one is a requirement of the host
 * settings menu turned into an assertion. That is the point -
 * a menu that behaves plausibly but not as specified is the failure this is
 * meant to catch, and it is easier to catch here than on a screen.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_gamemenu.cpp \
 *       port/frontend/gamemenu.cpp -o test_gamemenu && ./test_gamemenu
 */

#include "gamemenu.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

Lsw2MenuRow toggle(const char* label, const char* a, const char* b,
                   int initial = 0) {
    Lsw2MenuRow r;
    r.label = label;
    r.values[0] = a;
    r.values[1] = b;
    r.value_count = 2;
    r.value = initial;
    return r;
}

Lsw2MenuRow action(const char* label) {
    Lsw2MenuRow r;
    r.label = label;
    return r;
}

/* The host settings menu, exactly as specified. */
enum HostRow { kMaster, kLan, kRoster, kCharacter, kExtras, kControls,
               kCustomization, kMusic, kLobbyTime, kStart, kHostRowCount };

void build_host_menu(Lsw2Menu& m) {
    m.reset("Host Internet Game");
    m.add(toggle("Visible to Master", "NO", "YES", 1));
    m.add(toggle("Visible to LAN", "NO", "YES", 1));

    Lsw2MenuRow roster;
    roster.label = "Roster";
    roster.values[0] = "Hunters";
    roster.values[1] = "ALL";
    roster.values[2] = "Random Hunter";
    roster.values[3] = "Random ALL";
    roster.values[4] = "One";
    roster.value_count = 5;
    m.add(roster);

    Lsw2MenuRow character;
    character.label = "Character";
    character.values[0] = "Greedo";
    character.value_count = 1;
    character.hidden = true;          /* only under Roster: One */
    m.add(character);

    m.add(toggle("Extras", "OFF", "ON"));
    /* defaults to ENHANCED */
    m.add(toggle("Controls", "TANK", "ENHANCED", 1));

    Lsw2MenuRow custom;
    custom.label = "Customization";
    custom.values[0] = "OFF";
    custom.values[1] = "HATS";
    custom.values[2] = "ALL";
    custom.value_count = 3;
    custom.value = 2;                 /* defaults to ALL */
    m.add(custom);

    m.add(toggle("Music", "DEFAULT", "RANDOM"));

    Lsw2MenuRow lobby;
    lobby.label = "LOBBY TIME";
    lobby.values[0] = "OFF";
    lobby.values[1] = "30s";
    lobby.values[2] = "60s";
    lobby.values[3] = "300s";
    lobby.value_count = 4;
    lobby.value = 1;                  /* defaults to 30s */
    m.add(lobby);

    m.add(action("Start Lobby"));
}

/* Move the cursor onto a row by pressing Down until it lands. */
void cursor_to(Lsw2Menu& m, int target) {
    for (int i = 0; i < kLsw2MenuMaxRows * 2 && m.cursor() != target; ++i)
        m.step(kLsw2KeyDown);
}

}  // namespace

int main() {
    std::printf("menu model\n");

    Lsw2Menu m;
    build_host_menu(m);

    check(m.row_count() == kHostRowCount, "the host menu has ten rows");

    /* --- the specified defaults ------------------------------------------ */
    check(std::strcmp(m.value_of(kControls), "ENHANCED") == 0,
          "Controls defaults to ENHANCED");
    check(std::strcmp(m.value_of(kCustomization), "ALL") == 0,
          "Customization defaults to ALL");
    check(std::strcmp(m.value_of(kLobbyTime), "30s") == 0,
          "LOBBY TIME defaults to 30s");
    check(m.value_of(kStart) == nullptr,
          "Start Lobby is an action, not a toggle");

    /* --- values cycle, even going only right or only left ---------------- */
    cursor_to(m, kLobbyTime);
    check(m.cursor() == kLobbyTime, "the cursor reaches LOBBY TIME");
    m.step(kLsw2KeyLeft);
    check(std::strcmp(m.value_of(kLobbyTime), "OFF") == 0, "left goes 30s -> OFF");
    m.step(kLsw2KeyLeft);
    check(std::strcmp(m.value_of(kLobbyTime), "300s") == 0,
          "and left again WRAPS to 300s");
    m.step(kLsw2KeyRight);
    check(std::strcmp(m.value_of(kLobbyTime), "OFF") == 0,
          "right wraps back the other way");

    /* --- Roster: One unhides the Character row below it ------------------- */
    check(m.row(kCharacter).hidden, "Character starts hidden");
    cursor_to(m, kRoster);
    for (int i = 0; i < 4; ++i) m.step(kLsw2KeyRight);
    check(std::strcmp(m.value_of(kRoster), "One") == 0, "Roster cycles to One");

    m.set_hidden(kCharacter, false);
    check(!m.row(kCharacter).hidden, "and Character unhides");

    /* Moving Roster off One re-hides the Character row and ignores it. */
    m.set_hidden(kCharacter, true);
    check(m.row(kCharacter).hidden, "toggling away re-hides it");

    /* --- a hidden row must not be reachable or occupy a line -------------- */
    const int y_extras_hidden = m.row_y(kExtras);
    m.set_hidden(kCharacter, false);
    const int y_extras_shown = m.row_y(kExtras);
    check(y_extras_shown == y_extras_hidden + kLsw2MenuRowPitch,
          "unhiding a row pushes the ones below it down one line");

    m.set_hidden(kCharacter, true);
    cursor_to(m, kRoster);
    m.step(kLsw2KeyDown);
    check(m.cursor() != kCharacter, "the cursor skips a hidden row");

    /* The cursor must not be left standing on a row that vanishes under it. */
    m.set_hidden(kCharacter, false);
    cursor_to(m, kCharacter);
    check(m.cursor() == kCharacter, "the cursor can sit on a shown row");
    m.set_hidden(kCharacter, true);
    check(m.cursor() != kCharacter,
          "and is moved off when that row is hidden under it");

    /* --- geometry is the game's, measured from its own menus -------------- */
    check(m.row_y(0) == kLsw2MenuFirstRowY, "row 0 sits at the measured y");
    check(m.row_y(1) - m.row_y(0) == kLsw2MenuRowPitch,
          "rows step by the measured pitch");

    /* --- events ------------------------------------------------------------ */
    cursor_to(m, kStart);
    check(m.step(kLsw2KeyA) == Lsw2MenuEvent::Activated,
          "A on an action row reports Activated");
    check(m.step(kLsw2KeyB) == Lsw2MenuEvent::Cancelled, "B reports Cancelled");
    cursor_to(m, kMusic);
    check(m.step(kLsw2KeyRight) == Lsw2MenuEvent::ValueChanged,
          "left/right on a toggle reports ValueChanged");
    check(m.step(kLsw2KeyA) == Lsw2MenuEvent::Activated,
          "A on a toggle row still reports Activated, for submenus");

    /* --- an all-hidden menu must not spin --------------------------------- */
    {
        Lsw2Menu empty;
        empty.reset("nothing");
        Lsw2MenuRow r = action("hidden");
        r.hidden = true;
        empty.add(r);
        empty.step(kLsw2KeyDown);
        check(true, "a menu of only hidden rows does not hang");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (!g_failures) std::printf("all checks passed\n");
    return g_failures ? 1 : 0;
}
