/* The Bounty Hunter lobby console's "Join by IP/Room Code".
 *
 * The lobby console has three arena-menu entries and this is the second. The
 * open question was HOW an address gets entered on a d-pad, and the design
 * chosen is: a room code in-game, plus a direct IP from the Multiplayer tab.
 *
 * So this screen enters a ROOM CODE - four characters, which a d-pad handles
 * without inventing an on-screen keyboard - and offers the direct address the
 * player set on the imgui Multiplayer tab as a second way in. That is the
 * general rule applied rather than reinvented: the game carries what can
 * reasonably be set there, imgui carries what cannot, and a full IPv4 address
 * typed one character at a time is squarely the latter.
 *
 * THE ALPHABET IS THE SERVICE'S. `services/master-server-selfhost` issues
 * Crockford base32 codes, so the picker offers exactly those thirty-two
 * characters - I, L, O and U are absent because Crockford omits them to stop
 * people confusing them with 1, 1, 0 and V. Offering a character the directory
 * can never issue would be offering a code that can never resolve.
 */

#ifndef LSW2_JOINMENU_H
#define LSW2_JOINMENU_H

#include "gamemenu.h"

/* Four characters plus a terminator. */
constexpr int kLsw2RoomCodeLength = 4;

enum Lsw2JoinRow {
    kJoinChar0 = 0,
    kJoinChar1,
    kJoinChar2,
    kJoinChar3,
    kJoinConnect,      /* "Connect" - resolve the code and join   */
    kJoinDirect,       /* "Join <address>" - the Multiplayer tab's */
    kJoinReturn,
    kJoinRowCount
};

struct Lsw2JoinState {
    /* The code being entered. Indices into the alphabet rather than characters,
     * because that is what the menu's `value` is; `text` is what gets drawn. */
    char code[kLsw2RoomCodeLength + 1] = "0000";

    /* One-character strings the menu rows point at. The row model takes a
     * `const char*` and does not copy, so these have to live as long as the
     * menu does - which is why they are here and not in the builder. */
    char cell[kLsw2RoomCodeLength][2] = {{'0', 0}, {'0', 0}, {'0', 0}, {'0', 0}};

    /* "Join 192.168.1.10:28550", built by sync() from the imgui settings so the
     * row says where it would actually go rather than promising vaguely. */
    char direct[48] = "Join by IP";
};

/* The Crockford alphabet the master server issues from. */
const char* lsw2_room_code_alphabet();
int lsw2_room_code_alphabet_size();

/* Where `c` sits in that alphabet, or 0 when it is not in it. */
int lsw2_room_code_index(char c);

void lsw2_join_menu_build(Lsw2Menu& menu, const Lsw2JoinState& s);

/* Pull the four characters back out of the menu into `s.code`, and refresh the
 * direct-connect row's label from the given address. */
void lsw2_join_menu_sync(Lsw2Menu& menu, Lsw2JoinState& s,
                         const char* address, int port);

#endif /* LSW2_JOINMENU_H */
