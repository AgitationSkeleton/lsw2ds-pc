/* Player names, made safe to draw and safe to send.
 *
 * The master server list shows the player's name, sanitized to avoid
 * injections and other security problems, and names also appear in two other
 * places: the join and disconnect popups ("<Playername> has joined the game!")
 * and the host's kick list.
 *
 * WHERE THE DANGER IS, and it is not the master server. `lsw2_master.cpp`
 * already JSON-escapes on the way out and the directory allow-lists on the way
 * in, so that path is covered twice. The gap is the OTHER direction: a name that
 * arrives from a PEER over the game's own transport, which no service has
 * inspected, and which the fork then draws into the game's frame and stores in
 * a fixed 24-byte slot.
 *
 * Nothing sends names over that transport yet - `Lsw2ForkSession::tick` invents
 * "Player N" until the handshake exists - so this is in place before the thing
 * it defends against, which is the only order that ever works.
 *
 * AN ALLOW-LIST, NOT A DENY-LIST. Printable ASCII and nothing else. The game's
 * own strings do contain accented characters, so this is narrower than the font
 * can draw - but a name is not prose, every renderer downstream is ours or the
 * game's, and the failure mode of being too narrow is a mangled name where the
 * failure mode of being too wide is a control character in a menu.
 */

#ifndef LSW2_NAME_H
#define LSW2_NAME_H

#include <cstddef>

/* Copy `in` into `out` keeping only printable ASCII, collapsing runs of spaces,
 * and trimming both ends. Always writes a terminated string.
 *
 * An empty or entirely unprintable name becomes `fallback`, because a blank row
 * in a kick list is worse than a placeholder - the host cannot tell whether it
 * is a player or a drawing bug. Pass nullptr for `fallback` to allow empty.
 *
 * Returns the number of characters written, not counting the terminator. */
size_t lsw2_sanitise_name(const char* in, char* out, size_t cap,
                          const char* fallback);

#endif /* LSW2_NAME_H */
