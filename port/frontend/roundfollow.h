/* A CLIENT FOLLOWS THE HOST INTO THE ARENA - the online lobby's round, on every
 * machine rather than only the one that pressed the button.
 *
 * MEASURED with two instances, host and joiner:
 *
 *     HOST    [diag] level  9 -> 4   Jabba's Tower (Arena)
 *     CLIENT  [diag] level  0 -> 9   Cantina Arena Lobby      ... and stayed
 *
 * The host warped and the client sat in the lobby. Nothing told it: despite
 * its name, `lsw2_netmsg_start_round` writes message 0x0A into the LOCAL
 * guest's receive buffer and sends nothing to anybody. A comment in fork.cpp
 * claiming "the message still goes out for the OTHER machines" was simply
 * wrong, and is corrected there.
 *
 * SO THE ROUND RIDES THE CHANNEL THAT ALREADY WORKS. The host broadcasts its
 * settings and clients adopt them - forksession.cpp's own note calls that "THE
 * CLIENT ADOPTS THE HOST'S RULES" - so `Lsw2HostSettings::round_level` carries
 * the arena level, and this watches it.
 *
 * AND IT IS THE SAME FIELD "JOIN IN PROGRESS" NEEDS. A match already
 * under way may be joined, and a player who arrives as a round begins is
 * given, automatically, the hunter they had in the lobby. A joiner adopts
 * the settings on arrival, sees a round is running, and follows - which is the
 * same code path as following a round that starts while already connected. One
 * mechanism, two clauses.
 *
 * THE WARP IS lsw2_cheats_warp, the mechanism the fork has used on every boot
 * since it existed and the one that made the host's own round start work. Nothing
 * new is invented here; the client does what the host did, when it is told to.
 */

#ifndef LSW2_ROUNDFOLLOW_H
#define LSW2_ROUNDFOLLOW_H

/* Once a frame from the fork tick. Does nothing on the host, nothing offline,
 * and nothing while the announced level matches where this machine already is.
 *
 * HELD UNTIL THE WARP IS ACCEPTED. lsw2_cheats_warp refuses while the level
 * state is mid-construction, which is exactly when a client that has just been
 * told about a round is likely to ask - so a refusal retries next frame rather
 * than dropping the round. */
void lsw2_roundfollow_tick();

#endif /* LSW2_ROUNDFOLLOW_H */
