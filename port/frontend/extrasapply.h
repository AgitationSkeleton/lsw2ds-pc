/* The host settings' Extras, applied to the running game.
 *
 * Extras can be stacked, and are communicated to clients when they join. This
 * is also why the pause menu's own Extras screen is hidden: in the Bounty
 * Hunter mode, which Extras are on is decided by the host for the host's
 * match.
 *
 * WHAT WAS ALREADY THERE AND WHAT WAS NOT. The host's ten-row menu had the
 * Extras row and its submenu; the eighteen flags persisted across runs; the
 * mask travelled to clients in the settings packet. Every part of the feature
 * existed except the part where an Extra does something. A host could switch on
 * Disco, have it remembered, have it sent - and play a match with no Disco.
 *
 * That is the shape this project keeps finding: a feature complete in every
 * layer except the one that touches the guest.
 *
 * WHERE IT GOES. `+0x4518` in the save block is the ENABLED array, as opposed to
 * `+0x4514`, which is what the Cantina terminal has SOLD you. The
 * everything-unlocked-from-the-start unlock fills
 * the second and deliberately not the first - cheats.cpp says so in as many
 * words - because which Extras are on is the host's call.
 *
 * The index question, and why it stopped this being built earlier, is in
 * cheats.cpp: one note said the two arrays did not share an index
 * space, and the Extras terminal's own toggle handler shows that they do.
 *
 * WHY EVERY FRAME IS WRONG AND ON CHANGE IS RIGHT. The array is the game's own,
 * and the game writes it too - the Cantina terminal, and message 0x36 from
 * another DS. Rewriting all eighteen bits sixty times a second would make the
 * fork win every one of those races silently. Writing only when the host's
 * settings actually change leaves the game's own writes alone in between, which
 * also means a mid-match change by the host lands exactly once. */

#ifndef LSW2_EXTRASAPPLY_H
#define LSW2_EXTRASAPPLY_H

#include <cstdint>

/* THE PURE HALF: has the host's set changed since we last wrote it?
 *
 * `mask` is the eighteen flags packed low bit first, which is the form
 * `lsw2_host_extras_mask()` already produces for the settings file and the
 * wire - so there is one packing in the port rather than two that have to agree.
 *
 * `applied` is what was last written, or `kLsw2ExtrasNever` if nothing has been.
 * Split out so the "write once per change, and once on arrival" rule is testable
 * without a guest. */
constexpr uint32_t kLsw2ExtrasNever = 0xFFFFFFFFu;

bool lsw2_extras_should_write(uint32_t mask, uint32_t applied, bool in_fork);

/* Call once a frame. Silent in the baseline build, and silent until the host's
 * Extras differ from what was last written. */
void lsw2_extras_tick();

#endif /* LSW2_EXTRASAPPLY_H */
