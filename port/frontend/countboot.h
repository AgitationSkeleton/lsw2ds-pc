/* Route 2 for the online lobby's player-count blocker: the game's own count
 * bootstrap, with the real count.
 *
 * WHY THIS EXISTS AND WHY IT IS NOT A WORKAROUND. An earlier reading concluded
 * that the three player-count words are conclusions the game derives from the
 * view mask and that writing them is futile. A later one found that too
 * absolute:
 * `0x020700DC` and `0x020727A8` are the game's OWN two-player bootstrap and set
 * all three directly, to a literal 2, with no mask and no popcount -
 *
 *     LDR r0, =0x021027F8 ; MOV r1, #2 ; BL 0x0205CE5C   the input count
 *     MOV r0, #2          ;              BL 0x020730D0   the slot bound
 *     MOV r0, #2          ;              BL 0x02073054   the player count
 *
 * One of the two sits behind a state vtable slot; the derivation lives in a
 * different state's tick. So the counts have two mechanisms and which is live
 * depends on the state.
 *
 * This reproduces that sequence exactly - same three setters, same order,
 * through the game's own code - with the fork's real player count instead of 2.
 * The only difference from what the cartridge does is the number, and the port
 * knows the right one.
 *
 * WHY IT MATTERS THAT IT IS THE SETTERS AND NOT THREE STORES. `0x02073054` does
 * more than store: it reconfigures the view manager when the multi-view bit is
 * set, which is the array the enhanced-controls camera work reads. Three bus writes
 * would set the words and skip that. Calling the setter is the difference
 * between doing what the game does and doing what it looks like.
 *
 * BEHIND A BUTTON, not automatic. Which of the three player-count routes is
 * right depends on whether the derivation runs, and the session's first log
 * line answers that. Doing this unprompted would be choosing before the evidence. */

#ifndef LSW2_COUNTBOOT_H
#define LSW2_COUNTBOOT_H

/* Arm the bootstrap. From the imgui button; takes effect on the next fork tick. */
void lsw2_countboot_arm();

/* Once a frame from the fork tick. Does nothing unless armed. */
void lsw2_countboot_tick();

#endif /* LSW2_COUNTBOOT_H */
