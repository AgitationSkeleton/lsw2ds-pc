/* The host settings menu's Roster rule.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_roster.cpp \
 *       port/frontend/roster.cpp port/frontend/characterpick.cpp \
 *       port/frontend/lasthunter.cpp port/frontend/hostsettings.cpp \
 *       port/frontend/gamemenu.cpp -o test_roster
 */

#include "roster.h"

#include "characterpick.h"
#include "lasthunter.h"

#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    std::printf("which rules decide the character, and which leave it alone\n");
    check("One decides", lsw2_roster_assigns_character(Lsw2Roster::One));
    check("Random Hunter decides",
          lsw2_roster_assigns_character(Lsw2Roster::RandomHunter));
    check("Random All decides",
          lsw2_roster_assigns_character(Lsw2Roster::RandomAll));
    check("Hunters leaves it to the player - the statue select",
          !lsw2_roster_assigns_character(Lsw2Roster::Hunters));
    check("and so does ALL",
          !lsw2_roster_assigns_character(Lsw2Roster::All));

    std::printf("\"any setting besides Hunters skips the character select\"\n");
    check("Hunters keeps the select", !lsw2_roster_skips_select(Lsw2Roster::Hunters));
    check("ALL skips it", lsw2_roster_skips_select(Lsw2Roster::All));
    check("One skips it", lsw2_roster_skips_select(Lsw2Roster::One));
    check("Random Hunter skips it",
          lsw2_roster_skips_select(Lsw2Roster::RandomHunter));
    check("Random All skips it",
          lsw2_roster_skips_select(Lsw2Roster::RandomAll));

    std::printf("One: everybody is the host's pick\n");
    Lsw2HostSettings s;
    s.roster = Lsw2Roster::One;
    s.character = 68;                     /* Gonk Droid */
    check("station 0 gets it", lsw2_roster_character(s, 0, 0) == 68);
    check("so does station 3", lsw2_roster_character(s, 3, 0) == 68);
    check("and it does not change on respawn",
          lsw2_roster_character(s, 2, 17) == 68);

    std::printf("the player-chooses rules assign nothing\n");
    s.roster = Lsw2Roster::Hunters;
    check("Hunters returns -1", lsw2_roster_character(s, 0, 0) == -1);
    s.roster = Lsw2Roster::All;
    check("ALL returns -1", lsw2_roster_character(s, 0, 0) == -1);

    std::printf("Random Hunter: a hunter, every time\n");
    s.roster = Lsw2Roster::RandomHunter;
    check("every result is one of the ten", [&] {
        for (int st = 0; st < 4; ++st)
            for (int sp = 0; sp < 400; ++sp)
                if (!lsw2_is_bounty_hunter(lsw2_roster_character(s, st, sp)))
                    return false;
        return true;
    }());
    check("all ten are reachable", [&] {
        bool seen[kLsw2HunterCount] = {false};
        for (int st = 0; st < 4; ++st)
            for (int sp = 0; sp < 400; ++sp) {
                const int i = lsw2_hunter_index(lsw2_roster_character(s, st, sp));
                if (i >= 0) seen[i] = true;
            }
        for (bool b : seen) if (!b) return false;
        return true;
    }());
    check("a respawn usually changes it - a new roll 'every time they spawn'",
          [&] {
              int changes = 0;
              for (int sp = 0; sp < 50; ++sp)
                  if (lsw2_roster_character(s, 1, sp) !=
                      lsw2_roster_character(s, 1, sp + 1))
                      ++changes;
              /* Ten hunters, so about one in ten repeats. Anything above half
               * would mean the hash is not mixing. */
              return changes > 25;
          }());

    std::printf("Random All: any offered character, never a custom slot\n");
    s.roster = Lsw2Roster::RandomAll;
    check("every result is on an offered page", [&] {
        for (int st = 0; st < 4; ++st)
            for (int sp = 0; sp < 2000; ++sp) {
                const int id = lsw2_roster_character(s, st, sp);
                if (id < 0) return false;
                bool found = false;
                for (int p = 0; p < kLsw2PickOfferedPages && !found; ++p)
                    for (int q = 0; q < kLsw2PickSlots; ++q)
                        if (kLsw2PickTable[p][q] == id) { found = true; break; }
                if (!found) return false;
            }
        return true;
    }());
    check("and never one of the three custom slots", [&] {
        for (int st = 0; st < 4; ++st)
            for (int sp = 0; sp < 2000; ++sp) {
                const int id = lsw2_roster_character(s, st, sp);
                if (id == 73 || id == 74 || id == 75) return false;
            }
        return true;
    }());
    check("non-hunters do come up - it is not the hunter table again", [&] {
        for (int sp = 0; sp < 200; ++sp)
            if (!lsw2_is_bounty_hunter(lsw2_roster_character(s, 0, sp)))
                return true;
        return false;
    }());

    std::printf("determinism - the property that stops a desync\n");
    check("the same inputs give the same character every call",
          lsw2_roster_character(s, 2, 9) == lsw2_roster_character(s, 2, 9));
    check("two stations on the same spawn differ",
          lsw2_roster_character(s, 0, 5) != lsw2_roster_character(s, 1, 5) ||
          lsw2_roster_character(s, 2, 5) != lsw2_roster_character(s, 3, 5));

    /* ADJACENT INPUTS MUST NOT COLLIDE. A naive station*31+spawn hash sends
     * (0,1) and (1,0) to neighbouring values, which land on the same character
     * once taken modulo ten - two players spawning together as the same hunter,
     * every time. */
    check("station and spawn are not interchangeable",
          lsw2_roster_mix(0, 1, 0) != lsw2_roster_mix(1, 0, 0));
    check("the two modes walk different sequences", [&] {
        Lsw2HostSettings a = s, b = s;
        a.roster = Lsw2Roster::RandomHunter;
        b.roster = Lsw2Roster::RandomAll;
        int same = 0;
        for (int sp = 0; sp < 100; ++sp)
            if (lsw2_roster_character(a, 0, sp) == lsw2_roster_character(b, 0, sp))
                ++same;
        return same < 40;
    }());

    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
