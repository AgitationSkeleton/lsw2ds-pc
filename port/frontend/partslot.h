/* Which slot a part occupies - the lookup character customization needs and
 * has never had.
 *
 * WHY THIS EXISTS. It was established that the customize slots at
 * `0x02107890 + 0xA6..0xB0` are read only when the character being built is id
 * 72, Mix N Match, and that no character on the fork's two Bounty Hunter pages
 * is id 72. The mechanism that CAN dress any character is the gear applier
 * `0x020063D4(actor, part, transform)`, which has no character-id test - it is
 * how Old West puts a cowboy hat on whoever is on screen.
 *
 * To substitute a part there, the port has to know what a part IS. This is
 * that: the game's own part-id -> descriptor -> slot resolution, transcribed.
 *
 * THE RESOLUTION, from `0x0208DAD8` and its categoriser `0x0208DA74`:
 *
 *   id range      category   table         stride
 *   0x00..0x2C       0       0x020F2414     0x28
 *   0x2D..0x6A       1       0x020F3E68     0x2C
 *   0x6B..0xB0       2       0x020F3490     0x24
 *   0xB1..0xE7       3       0x020F2B1C     0x2C
 *   0xE8..0xF9       4       0x020F2144     0x28
 *   0xFA and up      5       none - `0x0208DB64` returns 0
 *
 * and `descriptor + 8` is the SLOT, which `0x02006450` reads and then uses as a
 * jump-table index (`CMP r0,#4 / ADDLS pc, pc, r0, LSL #2`) and as the index
 * into the actor's own record of what it is wearing:
 *
 *   02006450  LDRB r0, [descriptor + 8]     the slot
 *   02006454  ADD  r0, actor, r0, LSL #1
 *   0200645C  STRH part, [r0 + 0x600 + 0xE8]
 *
 * so **`actor + 0x6E8 + slot*2` holds the part currently in that slot**, which
 * is the table `config/runtime_attachment_state_defaults.json` describes as
 * "the group's current selector" without having located it.
 *
 * CROSS-VALIDATED, not merely transcribed. `config/customization_modes.json`
 * recorded `slot_index: 3` for all eight headwear parts by a different method
 * months ago. Running those eight ids through the table arithmetic here gives
 * `descriptor + 8 == 3` for every one - two independent derivations agreeing on
 * data neither could have copied from the other. `test_partslot.cpp` pins the
 * eight, and the four range boundaries where an off-by-one would land in the
 * wrong table and silently return a plausible slot.
 *
 * WHAT THIS IS NOT. It is not the substitution, and not a decision about which
 * parts the fork should swap. It is the lookup those need, built and checked
 * first, because the last four customization attempts each began by writing
 * something. */

#ifndef LSW2_PARTSLOT_H
#define LSW2_PARTSLOT_H

#include <cstdint>

/* `0x0208DA74`. 0..4 for a real part, 5 for one the game refuses. */
int lsw2_part_category(int part_id);

/* The descriptor's guest address, or 0 for category 5 - matching
 * `0x0208DB64`, which returns a null descriptor rather than an out-of-range
 * pointer. Pure: the tables are static ROM data at fixed addresses. */
uint32_t lsw2_part_descriptor(int part_id);

/* The actor's record of what it currently wears in `slot`.
 * `actor + 0x600 + 0xE8 + slot*2`, from `0x02006454`. */
constexpr uint32_t kLsw2ActorSlotTable = 0x6E8u;
uint32_t lsw2_actor_slot_addr(uint32_t actor, int slot);

/* The slot a part occupies, or -1 when the part has no descriptor. Needs the
 * bus - the slot is a byte inside the descriptor, not derivable from the id. */
int lsw2_part_slot(int part_id);

/* Once a frame from the fork's tick, silent unless something changes.
 *
 * WHAT IT MEASURES, and why this rather than a fix. The proposed substitution
 * rests on three things being true at once, and nothing has yet shown
 * any of them: that the player's six customize choices are real part ids, that
 * they resolve to slots the arena's hunters also use, and that those hunters
 * are actually wearing something in those slots when they are built. This
 * prints all three side by side.
 *
 * If the hunters' slots are empty, a substitution has nothing to substitute FOR
 * and the design changes. That is worth one log line before it is worth any
 * code. */
void lsw2_partslot_probe();

#endif /* LSW2_PARTSLOT_H */
