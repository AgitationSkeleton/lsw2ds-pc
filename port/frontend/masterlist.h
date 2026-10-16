/* The Bounty Hunter lobby console's "View Master Server" - the lobby browser.
 *
 *   Each lobby on the list shows the host player's name (sanitized against
 *   injection), the player count out of 4, the ping, and the DS signal
 *   strength icon for the connection, e.g. "Maclunkey 1/4 96 <green_ds_icon>".
 *
 * Four fields and an icon, in that order. The first three are text and are
 * built here; the icon is a sprite and is left as a STRENGTH VALUE for the
 * renderer to draw, because the port's menu renderer draws glyphs and not
 * sprites yet. The art exists - the game uses that element on
 * its own pause and arena-select screens - so this stores what to draw rather
 * than inventing a text stand-in that would have to be removed later.
 *
 * THE PING IS MEASURED BY US, NOT REPORTED BY THE DIRECTORY. `Lsw2ServerEntry`
 * already says so: "A ping is between you and the host; a number from the
 * master server would be the distance to the master server." Until it has been
 * measured it is -1, and a row shows that as unknown rather than as zero -
 * zero would read as a perfect connection.
 */

#ifndef LSW2_MASTERLIST_H
#define LSW2_MASTERLIST_H

#include "gamemenu.h"

#include "../net/lsw2_master.h"

#include <vector>

/* How many lobbies fit before the list needs paging. The menu holds twelve rows
 * and one is Return, so eleven - but the directory can return more than that
 * and silently dropping the tail would be the kind of quiet truncation this
 * project keeps warning about, so the count is reported. */
constexpr int kLsw2MasterListMax = 11;

/* Signal strength, as the DS icon draws it: no bars through three. */
enum class Lsw2Signal { None = 0, Weak, Fair, Good };

struct Lsw2MasterRow {
    char text[48] = {0};        /* "Maclunkey 1/4 96"            */
    Lsw2Signal signal = Lsw2Signal::None;
    std::string address;
    int port = 0;
    bool lan = false;           /* found on the local network    */
};

struct Lsw2MasterListState {
    std::vector<Lsw2MasterRow> rows;
    int total = 0;              /* what the directory returned, before the cap */
    /* The base recomp's master-server presentation, and ONLY the base
     * recomp's. The fork mixes LAN entries into the list; the base draws them
     * green, at the top - so this is a request the
     * caller makes rather than a rule the list applies to everyone. */
    bool lan_first = false;
    /* Why the list is empty, when it is. An empty list and an unreachable
     * directory look identical on screen and are completely different problems,
     * so the browser says which. */
    const char* note = nullptr;
};

/* Ping to bars. Thresholds are round numbers rather than measured ones, and are
 * documented as such: nothing has measured what a DS-era arena feels like at a
 * given latency, so these are honest guesses at the boundaries and are in one
 * place to be changed when something does. */
Lsw2Signal lsw2_signal_for_ping(int ping_ms);

/* Format one directory entry into a row. Exposed for testing because the
 * row format is specified exactly and it is worth pinning. */
void lsw2_master_row_format(const Lsw2ServerEntry& entry, Lsw2MasterRow* out);

/* Fill `state` from what the client fetched.
 *
 * `lan_first` is the base recomp's presentation: LAN games sort to the top and
 * draw green. It is a STABLE partition, so within each group the directory's own
 * order survives - a list that reshuffled its non-LAN rows every refresh would
 * move the row the player was about to choose, which is the same reason
 * `open_master_list` fetches before it opens. */
/* `max_rows` is HOW MANY THE SCREEN CAN SHOW, and it is a parameter because
 * there are now two screens with different answers.
 *
 * `kLsw2MasterListMax` is 11 for the fork's own browser - twelve rows less
 * Return - and the CARTRIDGE's list (the base recomp's browser) holds
 * SIXTEEN. Baking the fork's number in meant the baseline could only ever show
 * eleven of them: a UI constraint from one screen silently shortening another,
 * which is the kind of leak that looks like a directory problem. */
void lsw2_master_list_set(Lsw2MasterListState& state,
                          const std::vector<Lsw2ServerEntry>& servers,
                          bool directory_reachable,
                          bool lan_first = false,
                          int max_rows = kLsw2MasterListMax);

void lsw2_master_list_build(Lsw2Menu& menu, const Lsw2MasterListState& state);

/* Which lobby a row refers to, or -1 for Return and for the note. */
int lsw2_master_list_pick(const Lsw2MasterListState& state, int row);

#endif /* LSW2_MASTERLIST_H */
