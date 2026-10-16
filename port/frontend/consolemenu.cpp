/* See consolemenu.h. */

#include "consolemenu.h"

#include <cstring>

namespace {

Lsw2MenuRow action(const char* label) {
    Lsw2MenuRow r;
    r.label = label;
    return r;
}

}  // namespace

void lsw2_arena_menu_build(Lsw2Menu& menu) {
    /* THE TITLE IS THE GAME'S OWN WORD FOR THIS SCREEN. The cartridge's wireless
     * menu is reached from the arena lobby, and calling it anything else would
     * make the fork's version read as a different feature rather than as the
     * same one with better options. */
    menu.reset("Arena");

    menu.add(action("Host Internet Game"));
    menu.add(action("Join by IP/Room Code"));
    menu.add(action("View Master Server"));
}

void lsw2_console_menu_build(Lsw2Menu& menu, const Lsw2ConsoleState& s) {
    menu.reset("Lobby");

    menu.add(action("Start Game"));
    menu.add(action("Change Settings"));

    Lsw2MenuRow show;
    show.label = "Show Room Code";
    show.values[0] = "OFF";
    show.values[1] = "ON";
    show.value_count = 2;
    show.value = s.show_room_code ? 1 : 0;
    menu.add(show);

    /* THE CODE SITS BELOW ITS OWN TOGGLE, which is what "toggles a visible
     * four-letter/digit room code on the screen below that setting" describes.
     *
     * It is a row rather than free-floating text so that it takes a line in the
     * layout - the rows beneath it move down when it appears, instead of it
     * overdrawing "Kick Player". */
    Lsw2MenuRow code;
    code.label = s.room_code[0] ? s.room_code : "NO CODE YET";
    code.hidden = true;              /* sync() decides */
    code.selectable = false;         /* information, not a choice */
    menu.add(code);

    menu.add(action("Kick Player"));
    menu.add(action("Shut Down Lobby"));
    menu.add(action("Return"));
}

void lsw2_console_menu_sync(Lsw2Menu& menu, Lsw2ConsoleState& s) {
    if (menu.row_count() < kConsoleRowCount) return;

    s.show_room_code = menu.row(kConsoleShowCode).value != 0;

    /* THE TOGGLE MUST SHOW A LINE.
     *
     * The "show room code" setting once did not display the room code on the
     * next line, and this is where: the row was hidden unless a code
     * had been ISSUED, so with no master server running the player flipped
     * the setting and nothing whatever happened on screen.
     *
     * The reasoning that used to sit here - that "----" where a code should
     * be reads as a bug - was answering the wrong question. A toggle that
     * does nothing visible reads as a bug too, and worse, because the player
     * cannot tell whether they mis-set it. So the row appears whenever the
     * toggle is on, and says plainly when there is nothing to show.
     *
     * Room codes are a master-server thing - they are issued through the
     * master server - so this does NOT invent a local code to fill the gap;
     * a substitute code would be a fake. It reports the absence instead. */
    menu.set_hidden(kConsoleRoomCode, !s.show_room_code);

    /* The code row is information, never a cursor stop. That is a property of
     * the ROW (selectable = false), not something patched up here - an earlier
     * version nudged the cursor at sync time and the test caught it letting the
     * player move onto the row between syncs. */
}

bool lsw2_kick_menu_build(Lsw2Menu& menu, const Lsw2ConsoleState& s) {
    if (s.peer_count <= 0) return false;

    menu.reset("Kick Player");
    for (int i = 0; i < s.peer_count && i < 3; ++i)
        menu.add(action(s.peers[i]));
    menu.add(action("Return"));
    return true;
}

int lsw2_kick_menu_peer(const Lsw2ConsoleState& s, int row) {
    if (row < 0 || row >= s.peer_count) return -1;   /* Return, or out of range */
    return row;
}
