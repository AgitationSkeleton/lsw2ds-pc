/* Naming the byte that freezes the player at the arena console.
 *
 * THE REPORT: *"the arena lobby console entity just freezes the player until
 * they jump when interacted with in hunter"*. That is progress and a bug at
 * once - the refusal is gone, so the command-169 waiver works and the door
 * opens; what opens does not give the player back.
 *
 * ONE EXPLANATION IS ALREADY RULED OUT. `lsw2_menuhost_guest_keys` returns 0
 * while a fork menu is open, so a player behind an invisible menu could not
 * jump either - and they can. Whatever holds them is the GUEST's own state.
 *
 * TWO STATIC HUNTS FAILED TO NAME IT. The first looked for a script command
 * that waits on the dialog and found none - and the script VM at `0x02067A98`
 * walks its commands to completion in one call, so a declined command cannot
 * leave a script hanging. The second looked for an interaction flag on the
 * actor and found `+0x6F4`, which turned out to be the disguise value the
 * wireless layer writes. Guessing a third time is worse than measuring once.
 *
 * SO THIS MEASURES. On the frame the console is interacted with it snapshots a
 * window of the actor's state, and a second later it prints only the bytes that
 * CHANGED and stayed changed. A byte that goes up at the interaction and never
 * comes down is the lock, by construction - and the diff is short enough to
 * read, where a raw dump of 0x700 bytes is not.
 *
 * WHY A WINDOW AND NOT THE WHOLE ACTOR. The parts, the position and the
 * animation counters all move every frame; a diff over everything would be
 * noise. `0x6B0..0x700` is where this project's own reading has put the
 * per-actor state bytes - the character id at `+0x6BB`, the action row at
 * `+0x6C3`, the five part slots at `+0x6E8`, the re-attach flag at `+0x6F3`,
 * the disguise at `+0x6F4` - so it is the range most likely to hold one more.
 *
 * IT IS A MEASUREMENT, NOT A FIX, and it is here rather than in a document
 * because the fix depends on what it says. Delete it once the byte is named. */

#ifndef LSW2_FREEZEWATCH_H
#define LSW2_FREEZEWATCH_H

#include <cstdint>

/* The window snapshotted, chosen from where this port's own decodes have put
 * the per-actor state. */
constexpr uint32_t kLsw2FreezeWatchFirst = 0x6B0u;
constexpr uint32_t kLsw2FreezeWatchLast = 0x700u;

/* Called from lsw2_fork_console_interact(). Takes the snapshot. */
void lsw2_freezewatch_arm();

/* Called once a frame from the fork tick. Prints the surviving diff a second
 * after arming, once, and then does nothing until armed again. */
void lsw2_freezewatch_tick();

#endif /* LSW2_FREEZEWATCH_H */
