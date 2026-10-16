/* Character customization's last requirement: seeing other players'
 * customizations.
 *
 *   A player spawns in the cantina lobby, sees the other connected players
 *   (the host among them), and sees both their own and the other players'
 *   customizations.
 *
 * WHY THIS WAS REFUSED TWICE AND IS NOW BUILT. An earlier pass declined to put
 * parts on the wire, correctly, because the customization block at
 * `0x02107890 + 0xA6` is a **single global set** - a remote player's parts had
 * nowhere to be applied, and sending them would have been data with no consumer.
 *
 * Two findings changed that:
 *
 *   first     a PER-PLAYER record exists: `0x02106FD8 + slot*0x40` holds the
 *             character and five part halfwords, four slots.
 *   this pass a READER exists: `0x02098BD4` packs each slot's character and five
 *             parts into a compact struct, and its one caller is in the arena
 *             state's enter. Parts written into a peer's slot are picked up by
 *             the game's own code.
 *
 * Both had to be true. Either alone would have been a place to put bytes nobody
 * reads, or a reader with nothing to read.
 *
 * FIVE, NOT SIX. The global block has six part slots and this record's
 * initialiser writes five; the sixth was left as an open question, and is
 * printed in the diagnostics rather than guessed. Sending a field whose
 * existence is unsettled would be inventing protocol, so this carries five and
 * says so.
 */

#ifndef LSW2_PEERPARTS_H
#define LSW2_PEERPARTS_H

/* How many parts travel. Deliberately not kLsw2CustomSlots - that is six, the
 * size of the LOCAL block, and this is the per-player record's five. The two
 * numbers being different is the whole point of naming this one separately. */
constexpr int kLsw2PeerParts = 5;

/* Once a frame, from the fork tick.
 *
 * Sends the local player's parts when they change, and writes whatever arrives
 * into the sending station's record. Silent offline and in the baseline. */
void lsw2_peerparts_tick();

#endif /* LSW2_PEERPARTS_H */
