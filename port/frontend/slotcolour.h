/* The top-screen portrait ring is drawn in the player's slot colour.
 *
 * THE REQUIREMENT, in two parts:
 *
 *   - When connected to a hunter lobby and not the host, the blue portrait ring
 *     updates its palette to match the player's slot (green, red or purple).
 *
 *   - The base LSW2 recomp needs the same during a netgame: from player two's
 *     POV the top-screen portrait is green, matching what player two sees in
 *     hunter mode.
 *
 * So this is BOTH BUILDS: the fork's hunter lobby and the baseline's co-op
 * netgame want the same thing, because it is the same HUD.
 *
 * THE COLOURS ARE MEASURED, NOT CHOSEN. `config/bounty_hud_layout.json`:
 *
 *     "Slot 1 blue, slot 2 green, slot 3 pink, slot 4 purple. These correspond
 *      to the OAM palettes 4, 5, 6, 7 decoded earlier from the sprite list."
 *
 * Note the third one: the requirement says "red" and that measurement said
 * **pink**.
 *
 * RESOLVED, AND "RED" WAS RIGHT. The two describe different
 * parts of the same sprite. Sampling slot 3 on the in-match bottom screen
 * (gpudump_arena) and again on the score screen (gpudump_select):
 *
 *     slot 3   ring stroke (251, 0, 8)     empty fill (227, 170, 178)
 *     slot 4   ring stroke (186, 0, 251)   empty fill (211, 170, 227)
 *
 * The stroke is pure red and pure purple; the FILL of an unoccupied slot is
 * pink and lavender. "Slot 3 pink" was reading the fill. The subject of this
 * feature is the portrait RING, so "red" is the precise naming for it, and the
 * note here previously implied the opposite. All four strokes, for the record: slot 1 (0,113,251) blue, slot 2
 * (24,211,0) green, slot 3 red, slot 4 purple.
 *
 * NO CODE CHANGES, because the palette index is 6 either way and that is what
 * this file writes. What changes is that a later reader is no longer told the
 * requirement's colour naming was loose when it was exact.
 *
 * WHY A PALETTE REWRITE AND NOT A CALL. The palette is a field of an OAM entry
 * - attribute 2, bits 12-15 - and the game sets it there. Writing that field is
 * the same operation the game performs, not a substitute for it. There is no
 * function to call: the search for one found `0x0206BC7C`, which composes
 * `palette << 12` twenty-seven times and belongs to the DIALOG box, reached
 * only from `0x0206B934`.
 *
 * WHAT BOUNDS THE PREDICATE, because "recolour palette 4" on its own would be
 * a guess with a blast radius. `bounty_hud_layout.json`'s in-match capture of
 * the top screen is explicit:
 *
 *     "No score, no timer, no ammo on the top screen. It is portrait plus
 *      hearts only."
 *
 * So on the MAIN engine, in a match, the only things drawn are the portrait
 * and four red hearts - and the hearts are not in the player-colour range. A
 * sprite whose palette is 4..7 on that screen is the ring.
 *
 * THAT PREDICATE IS TOO WIDE, AND THE CAPTURES SAY SO.
 *
 * === SUPERSEDED, KEPT BECAUSE THE REASONING WAS SOUND AND THE DATA WAS NOT ==
 * | AN UNRESOLVED CONTRADICTION IN THAT LAST STEP ... the raw dump agrees:  |
 * | in out/work/gpudump_arena/inst0/state.json, `oam_main` holds ZERO       |
 * | entries and `oam_sub` holds nineteen. If that is also true in a         |
 * | netgame, the top-screen ring is a tile layer or a 3D quad, no OAM       |
 * | rewrite can reach it, and the fix is a BG repalette instead. The        |
 * | capture is one frame of a single-player replay, so it cannot settle     |
 * | what a lobby draws. A live lobby run should come back with the answer.  |
 * ==========================================================================
 *
 * IT DID NOT NEED A LIVE RUN. It needed the other 286,096 capture frames.
 * "The capture" was the one summary `state.json` at the top of a capture
 * directory; `out/work` holds 286,097 per-frame ones. Sampling every 25th
 * across all 85 capture streams:
 *
 *     10,931 frames sampled
 *      6,229 have the MAIN engine's OBJ layer ENABLED
 *      5,332 carry a main-engine sprite in palette 4..7
 *      5,332 of those 5,332 - one hundred percent - carry ALL FOUR
 *
 * Not one observed frame matches "the only player-colour sprite is the ring".
 * What is actually there, in gpudump_lobbywait/inst0 f025386, is four 40x40
 * portrait cells at x=9, y=29/69/109/149, each built from a 32x32 plus an 8x32
 * right strip plus a 32x8 bottom strip, in four consecutive palettes: a slot
 * COLUMN. Repainting it would recolour three other players to the local
 * player's colour and destroy the identification the arena HUD's read_rows()
 * derives from
 * that same palette bank - the failure warned about below for the sub engine,
 * reached through the main one instead.
 *
 * So the tick counts distinct player colours first and declines when there is
 * more than one. The write is unchanged for the case the reasoning above
 * actually describes; that case has never been observed.
 *
 * AND THEN THE GAME WAS RUN, AND IT IS NOT A SPRITE.
 *
 * Every previous answer here came from recorded melonDS captures, which do not
 * record POWCNT1 - so none of them could say which engine was on which screen,
 * and this file said so. The running port answers both at once. In the Cantina,
 * with the blue ring and the four hearts plainly on the top screen:
 *
 *     POWCNT1 = 0x820F          display swap bit 15 = 1
 *       -> engine A (main) IS the top screen
 *     engine A  DISPCNT=0x00010358   BG0=1 BG1=1 BG2=0 BG3=0 OBJ=0
 *     OAM main: 0 live sprites
 *
 * The top screen's sprite layer is DISABLED and its OAM is EMPTY while the ring
 * is being drawn. No OAM rewrite can reach it, in this frame or any frame
 * shaped like it - and the tick's own decline line printed live, unprompted,
 * saying exactly that.
 *
 * SO THE WRITE BELOW IS DEAD AS A ROUTE TO THE RING, and the hunter-lobby half
 * of this feature needs
 * a BG REPALETTE. It is kept rather than deleted for one reason: it is still
 * correct for a screen that does put the ring in OAM, and deleting it would
 * delete the guard, the decline and the reporting that found this out. What it
 * must not do is be mistaken for the feature.
 *
 * WHAT IS STILL OPEN: which layer, and the attempt to settle it is worth as
 * much as the answer would have been. Two probes were run over the socket -
 * disable engine A's BG1, and recolour every main-BG palette entry near a ring
 * stroke to magenta. The ring was unchanged by both. NEITHER IS EVIDENCE: the
 * register and the palette were read back afterwards and both had reverted, so
 * the game rewrites DISPCNT and palette RAM every frame and an external loop at
 * ~30 writes a second loses to its 60. An experiment that cannot act looks
 * exactly like an experiment that acted and found nothing.
 *
 * So the next step is PORT-SIDE. arenahud.cpp already re-asserts a DISPCNT bit
 * from inside the frame loop for the starfield, with a comment saying why; the
 * same shape on engine A's BG0 and BG1 in turn names the layer in two frames.
 *
 * AND NOT BY POINTING THIS AT THE SUB ENGINE, which is where the nineteen
 * sprites actually are. Those are the BOTTOM screen's slot column, and the
 * arena HUD's read_rows() works out which player owns each row from exactly the palette
 * bank this code would overwrite. Repointing it would fail to move the
 * top-screen ring AND break the identification the mirrored column depends on.
 *
 * AND IT SAYS WHAT IT CHANGED. Every distinct OAM entry it recolours is logged
 * once, with its position and size, so a predicate that turns out to be too
 * wide is visible in one line rather than as a mysteriously tinted HUD. That is
 * the difference between a bounded inference and a guess.
 */

#ifndef LSW2_SLOTCOLOUR_H
#define LSW2_SLOTCOLOUR_H

#include <cstdint>

/* OAM is 0x800 bytes at 0x07000000: the main engine's 128 entries first, the
 * sub engine's at +0x400. Eight bytes per entry, attribute 2 at +4. */
constexpr uint32_t kLsw2OamBase = 0x07000000u;
constexpr uint32_t kLsw2OamEngineStride = 0x400u;
constexpr uint32_t kLsw2OamEntryStride = 8u;
constexpr uint32_t kLsw2OamAttr2 = 4u;

/* The four player colours, in slot order. */
constexpr int kLsw2FirstSlotPalette = 4;
constexpr int kLsw2SlotPalettes = 4;

/* Once a frame from the fork tick, in both builds. Does nothing when the
 * enhancement is off, when no session is connected, or when the local player is
 * slot 0 - whose colour is already the blue the game draws. */
void lsw2_slotcolour_tick();

/* ---------------------------------------------------------------- the policy
 *
 * Split so `test_slotcolour.cpp` can pin the rule without OAM or a session. */

/* The palette a given slot should wear, or -1 for a slot outside 0..3. */
int lsw2_slot_palette(int slot);

/* Should this sprite's palette be rewritten, and to what?
 *
 * Returns the new palette, or -1 to leave the sprite alone. `palette` is what
 * the sprite currently wears; only the player-colour range is touched, so
 * every other sprite on the screen is out of scope by construction rather than
 * by a position test that could drift. */
int lsw2_slotcolour_rewrite(int palette, int local_slot);

#endif /* LSW2_SLOTCOLOUR_H */
