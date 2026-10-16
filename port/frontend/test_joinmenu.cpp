/* The room-code entry screen.
 *
 *   g++ -std=c++20 port/frontend/test_joinmenu.cpp port/frontend/joinmenu.cpp \
 *       port/frontend/gamemenu.cpp -o test_joinmenu
 */

#include "joinmenu.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    std::printf("join by room code\n");

    /* --- the alphabet is the directory's ---------------------------------- */
    const char* a = lsw2_room_code_alphabet();
    check(lsw2_room_code_alphabet_size() == 32, "thirty-two characters");
    check(std::strchr(a, 'I') == nullptr, "no I - Crockford omits it");
    check(std::strchr(a, 'L') == nullptr, "no L");
    check(std::strchr(a, 'O') == nullptr, "no O");
    check(std::strchr(a, 'U') == nullptr, "no U");
    check(std::strchr(a, '1') && std::strchr(a, '0') && std::strchr(a, 'V'),
          "but the characters they are confused with are all present");

    check(lsw2_room_code_index('0') == 0, "'0' is index 0");
    check(a[lsw2_room_code_index('Z')] == 'Z', "and Z round-trips");
    check(lsw2_room_code_index('z') == lsw2_room_code_index('Z'),
          "lowercase finds the same row - a code is not case sensitive");
    check(lsw2_room_code_index('I') == 0,
          "a character the alphabet lacks falls back rather than indexing off it");

    /* --- the menu --------------------------------------------------------- */
    Lsw2JoinState s;
    std::strcpy(s.code, "AB12");
    Lsw2Menu m;
    lsw2_join_menu_build(m, s);

    check(m.row_count() == kJoinRowCount, "seven rows");
    check(m.row(kJoinChar0).value == lsw2_room_code_index('A'),
          "the first character starts where the code says");
    check(m.row(kJoinChar3).value == lsw2_room_code_index('2'),
          "and so does the last");
    check(m.row(kJoinChar0).value_count == 32,
          "each character row cycles the whole alphabet");
    check(m.row(kJoinChar0).text_value != nullptr,
          "and renders through text_value, not the six-slot array");

    /* THE POINT OF THE MODEL EXTENSION. `values` holds six; the alphabet is
     * thirty-two. Without text_value, value_of would index off the end of the
     * array as soon as the player pressed right seven times. */
    lsw2_join_menu_sync(m, s, "10.0.0.4", 28550);
    for (int i = 0; i < 10; ++i) m.step(kLsw2KeyRight);
    lsw2_join_menu_sync(m, s, "10.0.0.4", 28550);
    check(m.value_of(kJoinChar0) != nullptr,
          "ten steps right still renders a value");
    check(std::strlen(m.value_of(kJoinChar0)) == 1,
          "and it is one character");

    /* --- sync pulls the code back out ------------------------------------- */
    m.row(kJoinChar0).value = lsw2_room_code_index('Z');
    m.row(kJoinChar1).value = lsw2_room_code_index('9');
    m.row(kJoinChar2).value = lsw2_room_code_index('Q');
    m.row(kJoinChar3).value = lsw2_room_code_index('7');
    lsw2_join_menu_sync(m, s, "10.0.0.4", 28550);
    check(std::strcmp(s.code, "Z9Q7") == 0, "sync reads the code out of the rows");
    check(std::strlen(s.code) == 4, "and terminates it");

    /* --- the direct row says where it would go ---------------------------- */
    check(std::strcmp(s.direct, "Join 10.0.0.4:28550") == 0,
          "the direct row names the address rather than promising vaguely");
    check(m.row(kJoinDirect).label == s.direct,
          "and the row points at that storage");

    lsw2_join_menu_sync(m, s, "", 0);
    check(std::strstr(s.direct, "not set") != nullptr,
          "with no address set it says so instead of offering a blank join");

    /* --- an out-of-range value cannot corrupt the code -------------------- */
    m.row(kJoinChar0).value = 999;
    lsw2_join_menu_sync(m, s, "10.0.0.4", 28550);
    check(s.code[0] == '0', "a nonsense index falls back to the first character");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
