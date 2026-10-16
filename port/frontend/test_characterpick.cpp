/* The host settings menu's character picker.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_characterpick.cpp \
 *       port/frontend/characterpick.cpp -o test_characterpick
 */

#include "characterpick.h"

#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    std::printf("the table, against the ROM's reading of 0x020EDDB2\n");
    check("twelve pages of eight",
          kLsw2PickPages == 12 && kLsw2PickSlots == 8);
    check("page 6 opens with Greedo", kLsw2PickTable[6][0] == 20);
    check("page 9 is full", kLsw2PickTable[9][7] == 6);
    check("page 10 is one entry and padding",
          kLsw2PickTable[10][0] == 68 && kLsw2PickTable[10][1] == kLsw2CharacterNone);

    std::printf("the offered range excludes the custom slots\n");
    check("eleven pages are offered", kLsw2PickOfferedPages == 11);
    check("page 11 is the three highest ids, which is why",
          kLsw2PickTable[11][0] == 73 && kLsw2PickTable[11][1] == 74 &&
          kLsw2PickTable[11][2] == 75 &&
          kLsw2PickTable[11][3] == kLsw2CharacterNone);

    /* THE OFFERED ROSTER, counted off the table itself - what the host menu's
     * Character row and the Free Play grid both draw from. The fork-drawn
     * picker these checks used to exercise is gone (2026-09-24,
     * freeplaypick.h): the host picks on the cartridge's own grid. */
    std::printf("every offered character is in the table, and only those\n");
    check("the offered pages hold 65 characters", [] {
        int n = 0;
        for (int p = 0; p < kLsw2PickOfferedPages; ++p)
            for (int s = 0; s < kLsw2PickSlots; ++s)
                if (kLsw2PickTable[p][s] != kLsw2CharacterNone) ++n;
        return n == 65;
    }());
    check("Gonk Droid is among them - the classic absurd pick",
          kLsw2PickTable[10][0] == 68);
    check("and R5-D4, on page 8", kLsw2PickTable[8][3] == 67);

    std::printf("\n");
    if (g_failures) { std::printf("%d FAILED\n", g_failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
