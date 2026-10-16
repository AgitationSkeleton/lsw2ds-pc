/* Telling the cartridge how many players there are.
 *
 * WHY THIS IS THE KEYSTONE. An earlier reading held that the game's own message
 * receive pump - `0x020B0410`, called every frame from the state manager's tick -
 * is guarded by `[0x021027F8 + 0x29] > 1` (but see the correction below).
 * Everything the fork wants to do with the cartridge's own messages (the round
 * start, the pick, the score screen's four choices) runs through that pump. While the byte
 * stays at 1, none of it dispatches.
 *
 * WHAT THE BYTE IS, decoded rather than inferred. The earlier reading called it
 * "a player count, on the strength of a `> 1` guard and its neighbour being the
 * local station index", and flagged that as a reading. It is now read out of the
 * code:
 *
 *   0205CE44   the object's initialiser writes [+0x29] = 1, [+0x28] = 0,
 *              [+0x2A] = 0, right after a loop stepping 0x0A per player record
 *   0205CE5C   a one-instruction setter: STRB r1, [r0, #0x29]; BX lr
 *   0206F870   its caller: `cmp r1,#1 / bls skip` then compare against a stored
 *              count and set only if it differs
 *
 * A setter called only when the value exceeds one, guarded against re-setting
 * the same number, on an object whose four 0x0A-byte records are the players'
 * input. It is the player count.
 *
 * WHY THE PORT HAS TO WRITE IT. The fork's peers arrive over a TCP transport the
 * cartridge knows nothing about, so none of the game's own callers ever fire.
 * The byte stays 1, the pump never runs, and the whole message layer is inert -
 * which would have presented as "the round start does nothing".
 *
 * WHAT THIS DELIBERATELY DOES NOT DO. It does not fabricate input for the
 * stations it announces. Raising the count tells the game there are players; the
 * lockstep already supplies their blocks. If a station is announced and never
 * sends, that is the transport's problem to show, not this file's to paper over.
 */

/* CORRECTED. THIS MODULE'S ORIGINAL PREMISE WAS FALSE.
 *
 * It was written on the claim that the receive pump `0x020B0410` runs
 * "only when `[0x021027F8 + 0x29] > 1`". Reading the pump shows otherwise:
 *
 *     020B0424  BL   0x02073034     -> returns [0x020FEB88]
 *     020B0428  CMP  r0, #0
 *     020B042C  BLE  done
 *     020B04D8  BL   0x02073034     re-read as the loop bound
 *     020B04DC  CMP  r5, r0
 *
 * `0x021027F8 + 0x29` appears nowhere in it. The pump iterates stations
 * `0 .. [0x020FEB88]-1`, and that word is guard and bound both.
 *
 * So this module mirrors the session's count into a byte that is *a* count -
 * it has its own setter at `0x0205CE5C` - but not the one that enables message
 * delivery. It is left in place because it is harmless and may still be the
 * right byte for the input records it sits among, and REMOVED FROM THE STORY
 * about the online lobby's message layer working.
 *
 * WHAT THE FIX IS NOT: writing `[0x020FEB88]` instead. Its setter `0x02073054`
 * also reconfigures the camera manager when the multi-view bit is set, and a
 * direct write would skip that - which is precisely the array the
 * enhanced-controls camera work reads. That would be the next workaround rather
 * than the fix.
 */

#ifndef LSW2_NETCOUNT_H
#define LSW2_NETCOUNT_H

#include <cstdint>

/* Once a frame, from the fork's tick. Mirrors the session's seated player count
 * into the guest, and puts it back to 1 when the session ends.
 *
 * Fork only, and only while guest state is reachable. Silent unless the value
 * actually changes. */
/* THE INPUT OBJECT the pump reads its player count out of, and the offset of
 * that count. ONE COPY, because there were three files and two spellings:
 * netcount.cpp and countboot.cpp called the base kInputObject while
 * diagnostics.cpp called it kNetCounts, and the 0x29 offset was kPlayerCount
 * here and kNetPlayers there.
 *
 * The byte at +0x29 was read as the pump's own guard - the reading that it will
 * not read anything while it reads 1 (corrected above) - so it was treated as
 * the single most consequential number in the netcode, and it was written down
 * twice.
 *
 * +0x2A is the local station, kept beside it because they are
 * read from the same record a line apart. */
constexpr uint32_t kLsw2NetCountObject = 0x021027F8u;
constexpr uint32_t kLsw2NetPlayerCount = 0x29u;
constexpr uint32_t kLsw2NetLocalStation = 0x2Au;

void lsw2_netcount_tick();

/* What the guest currently believes, or -1 when unreachable. For the
 * diagnostics, which watched this byte before anything wrote it. */
int lsw2_netcount_guest();

#endif /* LSW2_NETCOUNT_H */
