/* The host settings menu's Roster rule: who each player is in a match.
 *
 *   Roster: <Hunters/ALL/Random Hunter/Random ALL/One>
 *
 * Random Hunter has players play as a random hunter every time they spawn.
 * Random ALL has them play as any random character every time they spawn,
 * except the three custom character slots. Any setting besides Hunters skips
 * the character select phase of the arena flow.
 *
 * TWO OF THE FIVE ARE NOT THIS FILE'S BUSINESS. Hunters and ALL leave the
 * choice to the player - Hunters through the cartridge's own statue select, ALL
 * through the same screen unrestricted - so there is nothing for the port to
 * decide. The other three name a character, and that is what this computes.
 *
 * WHY IT IS DETERMINISTIC RATHER THAN RANDOM.
 *
 * This is the part worth reading before changing anything here. A match is
 * lockstep: every machine simulates every player. If each client rolled its own
 * die for "a random hunter", the host would see Greedo where a client saw
 * Bossk, and they would diverge on the first frame either of them mattered.
 *
 * So the roll is a FUNCTION of values every machine already agrees on - the
 * station and a spawn counter that comes out of the synchronised frame stream -
 * and no machine ever has to tell another what it rolled. Same inputs, same
 * character, everywhere, with no packet.
 *
 * That also means the sequence is reproducible from a log, which is the
 * difference between a desync report that can be chased and one that cannot.
 */

#ifndef LSW2_ROSTER_H
#define LSW2_ROSTER_H

#include "hostsettings.h"

#include <cstdint>

/* Does this roster rule decide the character, or does the player? */
bool lsw2_roster_assigns_character(Lsw2Roster roster);

/* Whether the arena's character-select phase is skipped.
 *
 * Any setting besides Hunters skips the character select phase. The
 * specification once called this row "Character" where every other mention
 * calls it "Roster", and Hunters is a Roster value, not a Character one, so
 * it is read as the row it names values from. */
bool lsw2_roster_skips_select(Lsw2Roster roster);

/* The character `station` should be for its `spawn`th appearance, or -1 when
 * the rule leaves the choice to the player.
 *
 * `spawn` must be a count every machine agrees on. Anything derived from the
 * lockstep frame stream qualifies; a local frame counter does not. */
int lsw2_roster_character(const Lsw2HostSettings& s, int station, int spawn);

/* The hash behind the two random modes. Exposed because a distribution is worth
 * testing and because a desync investigation will want to reproduce one. */
uint32_t lsw2_roster_mix(int station, int spawn, uint32_t salt);

/* Once a frame in the fork: hold the player to whatever the roster rule says.
 * Implemented in rosterenforce.cpp, which is separated from the pure logic
 * above so this file links into a test without the guest. */
void lsw2_roster_enforce();

/* Put the local player's character on the wire. Every frame in the fork, lobby
 * and match alike. */
void lsw2_roster_publish();

/* Tell the session whether the player is on an arena map. */
void lsw2_roster_match_state();

/* Apply what the other stations say they are playing as, so every machine's
 * copy of the lobby shows the same people. The other half of lsw2_roster_publish
 * - one puts our character on the wire, this takes theirs off it. */
void lsw2_roster_sync_peers();

/* Forget the spawn count - on leaving a match, so the next one starts over. */
void lsw2_roster_enforce_reset();

#endif /* LSW2_ROSTER_H */
