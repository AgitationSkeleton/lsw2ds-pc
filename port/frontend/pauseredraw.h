/* Making the pause menu redraw after the port moves its cursor.
 *
 * THE BUG, reported three times and each time misdiagnosed as arithmetic:
 * "moving up to the resume button at the top doesnt re-highlight it unless you
 * go downward all the way".
 *
 * `lsw2_pause_escape` is correct. Walking it by hand from row 2 with UP gives
 * `from=1, last=2`, so `backward` is true, so it steps 1 -> 0 and returns
 * Resume. Two fixes were shipped for the arithmetic and the symptom did not
 * move, which should have been the signal that the arithmetic was never it.
 *
 * WHAT IS ACTUALLY WRONG IS WHEN THE CORRECTION HAPPENS. The port's hook fires
 * on `0x02066708` gated on `lr == 0x0208E9CC` - the string decode inside the
 * BUILDER's row loop at `0x0208E980`. So the cursor is corrected while the menu
 * is being rebuilt, row by row, and the highlight for a row is decided in the
 * same pass. Correcting the cursor during the loop that is already drawing from
 * it is a race the port cannot win from inside.
 *
 * The game never has this problem because it never moves the cursor without
 * telling the menu to rebuild afterwards. Its own touch path does exactly that:
 *
 *     0208e6e0  str r5, [r6, #0xec]      write the cursor
 *     0208e6f0  bl  #0x208e874           and rebuild
 *
 * So the port should do what the game does. `0x0208E874(menu, 0)` is the
 * cursor-moved rebuild - the same function, the same argument the touch path
 * passes.
 *
 * WHY IT IS DEFERRED A FRAME rather than called on the spot: the correction
 * site sits inside the very function being called, and a reentrant rebuild
 * would be a crash. One frame of the highlight on a hidden row is not visible.
 *
 * AND WHERE IT IS DEFERRED TO - corrected after it was found while chasing
 * crashes. This was a host guest call from the fork tick, performed
 * "when the guest is ready". From the fork tick the guest is usually halted, so
 * the call was refused frame after frame - measured at two seconds in one run
 * (the guestcall line: "refused by ... halted 1131"). The player's A on
 * Settings came first: the pause update built the Settings page on the
 * un-rebuilt menu, handing it the menu's own text widget (0x0208EC70: sb+0x3C),
 * and the page's first row draw jumped to 0x0FFFFFF0 (pending returns 0206B55C
 * 0206B04C 0208EC80 0208E7C4). The late rebuild then drew the pause rows over
 * the Settings page - the "Settings page draws over the old pause rows" glitch.
 *
 * So the rebuild is a gear hop pinned to the pause menu's own per-frame update
 * (0x0208E514): it runs on the guest's thread at the top of the next update,
 * before that update reads the pad - the order the game keeps when it moves
 * the cursor itself. When the hop is taken the update's own object (r0) must
 * still be the menu the request names, or the request is dropped
 * (lsw2_pause_rebuild_still_live) - a queued call is validated when it is
 * taken, not when it is made.
 *
 * IT TERMINATES. After the rebuild the cursor is on a visible row, so the next
 * pass through the row loop finds nothing to correct and asks for nothing. */

#ifndef LSW2_PAUSEREDRAW_H
#define LSW2_PAUSEREDRAW_H

#include <cstdint>

/* `0x0208E874(menu, flavour)` - the menu rebuild. Flavour 0 is what the game
 * passes after moving the cursor; 1 is what the per-frame tick passes when the
 * menu is dirty for other reasons. */
constexpr uint32_t kLsw2PauseRebuild = 0x0208E874u;
constexpr uint32_t kLsw2PauseRebuildCursorMoved = 0u;

/* The pause menu's per-frame update, FUN_0208E514 - reached only through its
 * vtable, so its override fires every paused frame; it hosts the rebuild's hop
 * (dispatch_overrides.cpp, pause_state_update_override). */
constexpr uint32_t kLsw2PauseStateUpdate = 0x0208E514u;

/* Called from the cursor correction, with the menu object it corrected. Queues
 * the rebuild on the guest's thread for the top of the menu's next update; a
 * repeat before it runs is the same request. */
void lsw2_pause_request_redraw(uint32_t menu);

/* Asked by the hop when it takes a queued rebuild: is `menu` still the menu the
 * pause update is running (`update_object`, its r0)? Says so when it is not. */
bool lsw2_pause_rebuild_still_live(uint32_t menu, uint32_t update_object);

#endif /* LSW2_PAUSEREDRAW_H */
