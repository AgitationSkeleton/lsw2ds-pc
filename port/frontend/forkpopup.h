/* The online lobby's join and disconnect notices, in the game's own popup.
 *
 * When a player disconnects during gameplay or loses connection, it is handled
 * gracefully: they simply disappear from the HUD and from gameplay, and a popup
 * in the style of the game's own in-game time-limit popups reads
 * "<Playername> has disconnected...". When a player joins, the same kind of
 * popup reads "<Playername> has joined the game!".
 *
 * "In the style of the time-limit popups" is the whole specification, and until the
 * save gate was traced nobody knew what those were. They are **opcode 26 of the
 * level script's command VM**: one packed word, the message's string
 * id in argument zero. The fork already replaces the function that writes that
 * queue, in order to suppress one such popup - so producing one is the same
 * machinery pointed the other way.
 *
 * RETRACTED AS THE ROUTE. Producing one that way meant writing into
 * a script object's queue, and the object was whichever had last enqueued
 * anything - no popup raised that way was ever seen on screen (conn_p10b's
 * frames around a logged join popup show none). The game's own message display
 * was tried next and could not show one in a networked match. The fork now
 * draws the game's box itself - popuphud.h has both measurements - so the
 * three borrowed "XXXNOT USED" string ids and their rotation went with the
 * script route. What is left here is the wording and the sink.
 */

#ifndef LSW2_FORKPOPUP_H
#define LSW2_FORKPOPUP_H

#include <cstdint>

/* Longest message the fork will show. A name is capped at 24 by the sanitiser
 * and the suffixes are short, so this is comfortable rather than tight. */
constexpr int kLsw2PopupTextBytes = 64;

/* Build the two notice messages. Separate from raising them so the wording
 * is checkable without a running game - it is quoted text and getting it subtly
 * wrong is exactly the kind of thing nobody notices until it is on screen. */
void lsw2_popup_join_text(const char* name, char* out, int cap);
void lsw2_popup_disconnect_text(const char* name, char* out, int cap);

/* THE GUEST HALF, REACHED THROUGH A SINK rather than called directly.
 *
 * Showing a popup needs the frontend's present path (popuphud.h). The session -
 * which is what knows a peer arrived - must not depend on that: its unit test
 * links neither the framework nor the recompiler, and making it do so to test
 * menu navigation would be absurd.
 *
 * So the session announces and something else listens. With no sink installed
 * `lsw2_raise_popup` does nothing, which is exactly what a test wants and also
 * what the baseline build wants. */
using Lsw2PopupSink = void (*)(const char* text);

void lsw2_set_popup_sink(Lsw2PopupSink sink);
void lsw2_raise_popup(const char* text);

/* Convenience for the two notices: joined and disconnected. */
void lsw2_raise_join_popup(const char* name);
void lsw2_raise_disconnect_popup(const char* name);

#endif /* LSW2_FORKPOPUP_H */
