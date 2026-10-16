/* The lobby browser's rows, against the specified example row.
 *
 *   g++ -std=c++20 port/frontend/test_masterlist.cpp \
 *       port/frontend/masterlist.cpp port/frontend/gamemenu.cpp \
 *       port/net/lsw2_master.cpp -lws2_32 -o test_masterlist
 */

#include "masterlist.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

Lsw2ServerEntry entry(const char* name, int players, int ping) {
    Lsw2ServerEntry e;
    e.name = name;
    e.players = players;
    e.max_players = 4;
    e.ping_ms = ping;
    e.address = "10.0.0.4";
    e.port = 28550;
    return e;
}

}  // namespace

int main() {
    std::printf("master server list\n");

    /* --- THE SPECIFIED EXAMPLE --------------------------------------------
     * "Maclunkey 1/4 96 <green_ds_icon>" - so the text is exactly the
     * first three fields, and the icon is separate. */
    Lsw2MasterRow r;
    lsw2_master_row_format(entry("Maclunkey", 1, 96), &r);
    check(std::strcmp(r.text, "Maclunkey 1/4 96") == 0,
          "the row is the specified example, character for character");
    check(r.signal == Lsw2Signal::Fair, "96ms reads as a middling signal");
    check(r.address == "10.0.0.4" && r.port == 28550,
          "and it remembers where to connect");

    /* --- the signal thresholds -------------------------------------------- */
    check(lsw2_signal_for_ping(10) == Lsw2Signal::Good, "10ms is good");
    check(lsw2_signal_for_ping(60) == Lsw2Signal::Good, "60ms still good");
    check(lsw2_signal_for_ping(61) == Lsw2Signal::Fair, "61ms drops to fair");
    check(lsw2_signal_for_ping(120) == Lsw2Signal::Fair, "120ms still fair");
    check(lsw2_signal_for_ping(121) == Lsw2Signal::Weak, "121ms is weak");
    check(lsw2_signal_for_ping(9999) == Lsw2Signal::Weak, "and so is anything worse");

    /* UNMEASURED IS NOT BAD. -1 means nobody has pinged this host; showing an
     * empty meter for it would say "unreachable" about a lobby next door. */
    check(lsw2_signal_for_ping(-1) == Lsw2Signal::None, "unmeasured is its own state");
    lsw2_master_row_format(entry("Greedo", 2, -1), &r);
    check(std::strcmp(r.text, "Greedo 2/4 -") == 0,
          "and an unmeasured ping shows a dash, not a zero");

    /* --- an empty list says WHY ------------------------------------------ */
    Lsw2MasterListState s;
    lsw2_master_list_set(s, {}, true);
    check(s.rows.empty() && s.note != nullptr, "an empty list carries a note");
    check(std::strstr(s.note, "No lobbies") != nullptr,
          "reachable and empty means nobody is hosting");

    lsw2_master_list_set(s, {}, false);
    check(std::strstr(s.note, "Cannot reach") != nullptr,
          "unreachable says so instead - a different problem with a different fix");

    /* --- the menu --------------------------------------------------------- */
    std::vector<Lsw2ServerEntry> servers = {
        entry("Maclunkey", 1, 96), entry("Bossk", 3, 20)};
    lsw2_master_list_set(s, servers, true);
    check(s.note == nullptr, "a populated list has no note");

    Lsw2Menu m;
    lsw2_master_list_build(m, s);
    check(m.row_count() == 3, "two lobbies plus Return");
    check(std::strcmp(m.row(0).label, "Maclunkey 1/4 96") == 0, "row 0 is the first");
    check(std::strcmp(m.row(2).label, "Return") == 0, "and the last is Return");

    check(lsw2_master_list_pick(s, 0) == 0, "row 0 picks lobby 0");
    check(lsw2_master_list_pick(s, 1) == 1, "row 1 picks lobby 1");
    check(lsw2_master_list_pick(s, 2) == -1, "Return picks no lobby");

    /* --- the note row cannot be selected ---------------------------------- */
    lsw2_master_list_set(s, {}, true);
    lsw2_master_list_build(m, s);
    check(m.row_count() == 2, "the note plus Return");
    check(!m.row(0).selectable,
          "and the note is not selectable - pressing A on an explanation "
          "should do nothing");

    /* --- TRUNCATION IS REPORTED, NOT SILENT -------------------------------
     * The menu holds twelve rows. A directory with more lobbies than that has
     * to lose some, and `total` is what lets the caller say so rather than
     * quietly showing a subset as though it were everything. */
    std::vector<Lsw2ServerEntry> many;
    for (int i = 0; i < 20; ++i) many.push_back(entry("Host", 1, 50));
    lsw2_master_list_set(s, many, true);
    check(int(s.rows.size()) == kLsw2MasterListMax, "the list is capped");
    check(s.total == 20, "but the true count is kept");

    /* --- the signal icon reaches the menu ---------------------------------
     * `signal` was computed by lsw2_master_row_format from the day the browser
     * was written and read by NOTHING: the row text carried name, count and
     * ping, and the row's fourth element - the DS signal icon - had nowhere to go.
     * These pin the connection, because a value that travels nowhere is
     * exactly the kind of thing that goes unnoticed for another six passes. */
    {
        std::vector<Lsw2ServerEntry> servers;
        Lsw2ServerEntry good;  good.name = "Near";  good.players = 1;  good.ping_ms = 20;
        Lsw2ServerEntry fair;  fair.name = "Mid";   fair.players = 2;  fair.ping_ms = 150;
        Lsw2ServerEntry none;  none.name = "Quiet"; none.players = 0;  none.ping_ms = -1;
        servers.push_back(good); servers.push_back(fair); servers.push_back(none);

        Lsw2MasterListState st;
        lsw2_master_list_set(st, servers, true);
        Lsw2Menu menu;
        lsw2_master_list_build(menu, st);

        check(menu.row(0).badge == int(Lsw2Signal::Good),
              "a fast host carries three bars into the menu");
        check(menu.row(1).badge > 0 && menu.row(1).badge < int(Lsw2Signal::Good),
              "a slower one carries fewer");
        check(menu.row(2).badge == 0,
              "and an UNPINGED host carries none - not one bar, none");
        check(menu.row(menu.row_count() - 1).badge == 0,
              "Return has no icon");
    }


    /* --- the base recomp's browser: LAN first, in green -------------------
     * The base recomp's lobby list merges LAN games with the master server's
     * entries, putting LAN games first in green text - and the
     * fork does the OPPOSITE (LAN entries mixed in), so the default
     * must leave the order alone. Both halves are pinned. */
    {
        std::vector<Lsw2ServerEntry> mixed;
        for (int i = 0; i < 11; ++i) mixed.push_back(entry("Far", 1, 200));
        Lsw2ServerEntry near_ = entry("Nextdoor", 2, 5);
        near_.lan = true;
        mixed.push_back(near_);       /* LAST, and past the eleven-row cap */

        Lsw2MasterListState st;
        lsw2_master_list_set(st, mixed, true, true);
        check(st.rows.size() > 0 && st.rows[0].lan,
              "the LAN game is first even though it arrived last");
        /* THE CAP IS THE POINT. Ordering the finished rows instead of the
         * sources would have dropped this entry before it could be promoted. */
        check(int(st.rows.size()) == kLsw2MasterListMax,
              "the list is still capped");
        bool later_lan = false;
        for (size_t i = 1; i < st.rows.size(); ++i)
            if (st.rows[i].lan) later_lan = true;
        check(!later_lan, "and nothing else claims to be LAN");

        Lsw2Menu menu;
        lsw2_master_list_build(menu, st);
        check(menu.row(0).colour == kLsw2MenuGreen, "drawn green");
        check(menu.row(1).colour == 0, "an internet lobby keeps the menu colour");

        /* The fork's presentation: same input, no reordering, no colour. */
        Lsw2MasterListState fork;
        lsw2_master_list_set(fork, mixed, true);
        check(!fork.rows[0].lan, "the fork does NOT promote LAN games");
        Lsw2Menu fmenu;
        lsw2_master_list_build(fmenu, fork);
        check(fmenu.row(0).colour == 0, "and does not colour them");
    }


    /* --- the cap follows the SCREEN, not the fetch -------------------------
     * kLsw2MasterListMax is the fork's browser: twelve rows less Return. The
     * cartridge's list (the base recomp's browser) holds sixteen. Baking one
     * in meant the other silently showed fewer lobbies than it had room for -
     * a UI constraint from one screen shortening another, which would have read
     * as a directory problem. */
    {
        std::vector<Lsw2ServerEntry> many;
        for (int i = 0; i < 20; ++i) many.push_back(entry("Host", 1, 50));

        Lsw2MasterListState fork;
        lsw2_master_list_set(fork, many, true);
        check(int(fork.rows.size()) == kLsw2MasterListMax,
              "the fork's browser still caps at its own row count");

        Lsw2MasterListState game;
        lsw2_master_list_set(game, many, true, true, 16);
        check(int(game.rows.size()) == 16,
              "the cartridge's list gets all sixteen it can show");
        check(game.total == 20, "and the true count is still reported");

        Lsw2MasterListState none;
        lsw2_master_list_set(none, many, true, false, 0);
        check(none.rows.empty(), "a zero cap shows nothing rather than crashing");
        check(none.note != nullptr, "and says why the list is empty");
    }


    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
