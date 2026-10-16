/* The Bounty Hunter lobby console entity, the host's control panel inside the
 * lobby.
 *
 * The host interacts with the console entity in the arena lobby and gets these
 * options:
 *
 *   - "Start Game".
 *   - "Change Settings".
 *   - "Show Room Code" - off by default; toggles a visible four-letter/digit
 *     room code on the screen below that setting.
 *   - "Kick Player" - brings up a list of the connected players other than the
 *     host; pressing A on a player's name in that menu disconnects them.
 *   - "Shut Down Lobby" - ends communications and returns the host to the main
 *     lobby offline, until the host again interacts with the console entity by
 *     the hologram DS.
 *   - "Return" - closes the menu and returns the host to the still-running
 *     lobby; any settings that were changed are confirmed by doing this.
 *
 * HOST ONLY. These controls belong to the host; a client that walks
 * into the console gets nothing. That is not enforced here - this only builds
 * the menu - but it is why there is no client variant.
 *
 * THE ROOM CODE IS SHOWN, NOT CHOSEN. "Show Room Code" toggles whether the code
 * is drawn below its own row; the code itself comes from the master server
 * (services/master-server-selfhost issues four-character Crockford codes, which
 * is what "four-letter/digit" describes).
 */

#ifndef LSW2_CONSOLEMENU_H
#define LSW2_CONSOLEMENU_H

#include "gamemenu.h"

enum Lsw2ConsoleRow {
    kConsoleStart = 0,      /* "Start Game"       */
    kConsoleSettings,       /* "Change Settings"  */
    kConsoleShowCode,       /* "Show Room Code"   */
    kConsoleRoomCode,       /* the code itself, below that row */
    kConsoleKick,           /* "Kick Player"      */
    kConsoleShutDown,       /* "Shut Down Lobby"  */
    kConsoleReturn,         /* "Return"           */
    kConsoleRowCount
};

struct Lsw2ConsoleState {
    bool show_room_code = false;   /* off by default */
    /* Four characters plus a terminator. Empty until the master server has
     * issued one - a host on LAN only never gets a code, and the row stays
     * hidden rather than showing an invented one. */
    char room_code[8] = {0};
    /* Names of the connected players other than the host, for the kick list.
     * Four stations total, so at most three others. */
    char peers[3][24] = {{0}};
    /* WHICH STATION EACH NAME IS, because kicking needs one.
     *
     * Pressing A on a player's name in the kick menu DISCONNECTS that player,
     * and disconnecting is a transport operation keyed by
     * station, while the list is keyed by name. Without this the kick removed
     * the row and left the peer connected, still exchanging blocks with a host
     * that had stopped listing them.
     *
     * -1 means unknown, which is what a peer seated before this existed would
     * be; kicking one of those refuses rather than disconnecting a guess. */
    int peer_station[3] = {-1, -1, -1};
    int peer_count = 0;
};

/* THE ARENA LOBBY MENU - the three entries that replace the cartridge's.
 *
 * In place of the cartridge's "Host Multi-Card Game", "Host Single-Card Game"
 * and "Join Wireless Game", the in-game arena lobby menu presents "Host
 * Internet Game", "Join by IP/Room Code" and "View Master Server".
 *
 * WHY THE FORK DRAWS ITS OWN rather than relabelling the game's. The three
 * string ids are known and the fork does override them, so the cartridge's menu
 * would read correctly - but acting on a selection needs the code that handles
 * it, and which state class draws that menu was never identified despite five
 * attempts. A menu that reads right and does the wrong thing is worse than one
 * the port owns end to end.
 *
 * Drawing it ourselves costs nothing in presentation: gamemenu_render composites
 * into the DS frame in the game's own font, which matches the game's vanilla
 * presentation style.
 *
 * The relabels stay in place. If the cartridge's menu ever does appear, it says
 * the right words rather than offering multi-card play the fork cannot do. */
enum Lsw2ArenaRow {
    kArenaHost = 0,         /* "Host Internet Game"     */
    kArenaJoin,             /* "Join by IP/Room Code"   */
    kArenaMaster,           /* "View Master Server"     */
    kArenaRowCount
};

void lsw2_arena_menu_build(Lsw2Menu& menu);

void lsw2_console_menu_build(Lsw2Menu& menu, const Lsw2ConsoleState& s);

/* Keep the room-code row in step with the toggle above it, and with whether a
 * code exists at all. */
void lsw2_console_menu_sync(Lsw2Menu& menu, Lsw2ConsoleState& s);

/* Builds the list of connected players other than the host. Returns
 * false when nobody else is connected, so the caller can decline to open an
 * empty menu rather than showing one with only Return in it. */
bool lsw2_kick_menu_build(Lsw2Menu& menu, const Lsw2ConsoleState& s);

/* Which peer a row of the kick menu refers to, or -1 for Return. */
int lsw2_kick_menu_peer(const Lsw2ConsoleState& s, int row);

#endif /* LSW2_CONSOLEMENU_H */
