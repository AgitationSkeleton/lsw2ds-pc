/* The remembered last-played Bounty Hunter.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_lasthunter.cpp \
 *       port/frontend/lasthunter.cpp -o test_lasthunter
 *
 * The guest-reading half - lsw2_last_hunter_tick() - is not exercised here.
 * It needs a running level, and what it does once it has one is two lines over
 * lsw2_cheats_party_character(). What IS worth pinning down is everything
 * around it: the roster's contents against the ROM table, the membership test that
 * decides whether a value may be remembered at all, and the file-load path,
 * which is the one input that can arrive wrong.
 */

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
    std::printf("the roster, against the ROM's table at 0x020FFA00\n");
    check("ten entries", kLsw2HunterCount == 10);
    check("Greedo is entry 0 - which is why he is the default",
          kLsw2HunterIds[0] == 20 && kLsw2DefaultHunter == 20);
    check("Jango Fett is entry 9", kLsw2HunterIds[9] == 29);
    check("the table has no duplicates", [] {
        for (int i = 0; i < kLsw2HunterCount; ++i)
            for (int j = i + 1; j < kLsw2HunterCount; ++j)
                if (kLsw2HunterIds[i] == kLsw2HunterIds[j]) return false;
        return true;
    }());

    std::printf("the select screen's own pages, read from 0x020EDDB2\n");
    /* THE ROSTER'S ORDER AND THE SELECT SCREEN'S ORDER ARE DIFFERENT ORDERS
     * OVER THE SAME TEN CHARACTERS. An earlier reading inferred they were the same and
     * flagged the inference; reading the table disproved it. These checks are
     * what stop that assumption coming back. */
    int page = -1, slot = -1;
    check("Greedo opens page 6",
          lsw2_hunter_page_slot(20, &page, &slot) && page == 6 && slot == 0);
    check("Boba Fett is next to him, though he is roster entry 2",
          lsw2_hunter_page_slot(7, &page, &slot) && page == 6 && slot == 1 &&
          lsw2_hunter_index(7) == 2);
    check("Jango Fett is on page 6, NOT page 7 as the roster order implies",
          lsw2_hunter_page_slot(29, &page, &slot) && page == 6 &&
          lsw2_hunter_index(29) == 9);
    check("Lando Jabba's Palace closes page 6",
          lsw2_hunter_page_slot(34, &page, &slot) && page == 6 && slot == 7);
    check("page 7 is Leia then Zuckuss - the two the inference did not name",
          lsw2_select_character_at(7, 0) == 36 &&
          lsw2_select_character_at(7, 1) == 71);
    check("a non-hunter has no page", !lsw2_hunter_page_slot(1, &page, &slot));
    check("and the outputs are left alone when it fails", page == 6);

    std::printf("the pages hold exactly the roster, and nothing else\n");
    check("every hunter has a page and slot", [] {
        for (int i = 0; i < kLsw2HunterCount; ++i)
            if (!lsw2_hunter_page_slot(kLsw2HunterIds[i], nullptr, nullptr))
                return false;
        return true;
    }());
    check("the two pages hold ten characters and ten only", [] {
        int found = 0;
        for (int p = kLsw2HunterPageFirst; p <= kLsw2HunterPageLast; ++p)
            for (int s = 0; s < kLsw2SelectPageSlots; ++s) {
                const int id = lsw2_select_character_at(p, s);
                if (id < 0) continue;
                if (!lsw2_is_bounty_hunter(id)) return false;
                ++found;
            }
        return found == kLsw2HunterCount;
    }());
    check("page 7's padding reads as empty, not as character 100",
          lsw2_select_character_at(7, 2) == -1 &&
          lsw2_select_character_at(7, 7) == -1);
    check("a page outside the two is refused",
          lsw2_select_character_at(5, 0) == -1 &&
          lsw2_select_character_at(8, 0) == -1);
    check("so is a slot outside the eight",
          lsw2_select_character_at(6, -1) == -1 &&
          lsw2_select_character_at(6, 8) == -1);

    std::printf("membership - anything not in the table is not a hunter\n");
    check("Greedo is", lsw2_is_bounty_hunter(20));
    check("IG-88 is", lsw2_is_bounty_hunter(27));
    check("4-LOM is, and his id is ZERO - which a truthy test would miss",
          lsw2_is_bounty_hunter(0) && lsw2_hunter_index(0) == 1);
    check("a story character is not", !lsw2_is_bounty_hunter(1));
    check("a negative id is not", !lsw2_is_bounty_hunter(-1));
    check("the game's 'not available' sentinel is not",
          !lsw2_is_bounty_hunter(0x64));
    check("an unknown index is -1", lsw2_hunter_index(200) == -1);

    std::printf("remembering\n");
    check("starts at Greedo", lsw2_last_hunter() == kLsw2DefaultHunter);
    check("a hunter is remembered", lsw2_set_last_hunter(27));
    check("and read back", lsw2_last_hunter() == 27);
    check("the same one again is not a change", !lsw2_set_last_hunter(27));
    check("a non-hunter is refused", !lsw2_set_last_hunter(1));
    check("and does not overwrite what was there", lsw2_last_hunter() == 27);

    std::printf("the dirty flag, so the settings file is only rewritten when "
                "there is something to write\n");
    check("a change marks it", lsw2_last_hunter_dirty());
    lsw2_last_hunter_mark_clean();
    check("marking clean clears it", !lsw2_last_hunter_dirty());
    lsw2_set_last_hunter(1);
    check("a REFUSED write does not mark it dirty", !lsw2_last_hunter_dirty());
    lsw2_set_last_hunter(8);
    check("an accepted one does", lsw2_last_hunter_dirty());

    std::printf("loading from the settings file, which is hand-editable\n");
    lsw2_last_hunter_load(29);
    check("a valid id loads", lsw2_last_hunter() == 29);
    check("and a load is not a change to write back",
          !lsw2_last_hunter_dirty());
    lsw2_last_hunter_load(999);
    check("an impossible id falls back to Greedo rather than being carried",
          lsw2_last_hunter() == kLsw2DefaultHunter);
    lsw2_last_hunter_load(1);
    check("so does a real character who is not a hunter",
          lsw2_last_hunter() == kLsw2DefaultHunter);
    lsw2_last_hunter_load(0);
    check("but 4-LOM, whose id is 0, still loads",
          lsw2_last_hunter() == 0);

    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
