/* See joinmenu.h. */

#include "joinmenu.h"

#include <cstdio>
#include <cstring>

namespace {

/* CROCKFORD BASE32, and the four it leaves out are the point of it.
 *
 * No I, L, O or U. The first three because they are misread as 1, 1 and 0 when
 * someone reads a code aloud or copies it off a screen, and U because it turns
 * certain four-character codes into words nobody wants to read out. This is the
 * alphabet the directory issues from, so offering anything else would let a
 * player enter a code that cannot exist. */
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int kAlphabetSize = 32;

Lsw2MenuRow action(const char* label) {
    Lsw2MenuRow r;
    r.label = label;
    return r;
}

}  // namespace

const char* lsw2_room_code_alphabet() { return kAlphabet; }
int lsw2_room_code_alphabet_size() { return kAlphabetSize; }

int lsw2_room_code_index(char c) {
    /* Uppercased first, because a code is not case sensitive and a lowercase
     * letter arriving from a settings file should find its row rather than
     * silently becoming '0'. */
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (int i = 0; i < kAlphabetSize; ++i)
        if (kAlphabet[i] == c) return i;
    return 0;
}

void lsw2_join_menu_build(Lsw2Menu& menu, const Lsw2JoinState& s) {
    menu.reset("Join Game");

    /* ONE ROW PER CHARACTER. Four rows is more of the screen than a single
     * field would be, but a single field needs a cursor WITHIN the row - a
     * second axis of selection that the menu model does not have and that the
     * game's own menus never use. Four rows spends layout to avoid inventing an
     * interaction the player has not seen anywhere else in this game. */
    static const char* kLabels[kLsw2RoomCodeLength] = {
        "Code 1", "Code 2", "Code 3", "Code 4"};

    for (int i = 0; i < kLsw2RoomCodeLength; ++i) {
        Lsw2MenuRow r;
        r.label = kLabels[i];
        /* value_count is the whole alphabet and `values` is unused - that is
         * what text_value is for. `value` still cycles 0..31 on left and right
         * exactly as a two-value toggle cycles 0..1. */
        r.value_count = kAlphabetSize;
        r.value = lsw2_room_code_index(s.code[i]);
        r.text_value = s.cell[i];
        menu.add(r);
    }

    menu.add(action("Connect"));
    menu.add(action(s.direct));
    menu.add(action("Return"));
}

void lsw2_join_menu_sync(Lsw2Menu& menu, Lsw2JoinState& s,
                         const char* address, int port) {
    if (menu.row_count() < kJoinRowCount) return;

    for (int i = 0; i < kLsw2RoomCodeLength; ++i) {
        int v = menu.row(i).value;
        if (v < 0 || v >= kAlphabetSize) v = 0;
        s.code[i] = kAlphabet[v];
        s.cell[i][0] = kAlphabet[v];
        s.cell[i][1] = '\0';
    }
    s.code[kLsw2RoomCodeLength] = '\0';

    /* SAY WHERE IT WOULD GO. "Join by IP" is a promise; "Join 10.0.0.4:28550"
     * is a statement, and the difference matters when the player has forgotten
     * what they typed on a tab they cannot see from here. */
    if (address && *address)
        std::snprintf(s.direct, sizeof s.direct, "Join %s:%d", address, port);
    else
        std::snprintf(s.direct, sizeof s.direct, "Join by IP (not set)");

    menu.row(kJoinDirect).label = s.direct;
}
