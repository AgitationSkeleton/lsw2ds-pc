/* THE TOUCH SCREEN'S PLAYER PORTRAITS FOLLOWING A CHARACTER CHANGE.
 *
 * THE SYMPTOM: with random characters on, the arena HUD column on the top
 * screen showed every other player as the character they started the match
 * as, all match long, while they respawned as others. The column is a pixel
 * copy of the touch screen's per-player portraits (arenahud.h), so the copy
 * was faithful: the touch screen itself never changed.
 *
 * WHY IT NEVER CHANGES. The touch HUD (vtable 0x020FFA38) builds its portraits
 * once, in its init (vtable slot 2, 0x02099E98), and a cartridge match never
 * changes a player's character - the session roster's re-roll on every spawn
 * (sessionroster.h) is the fork's. For each slot with an actor the init does
 * (disassembled at 0x0209A208..0x0209A27C):
 *
 *     c    = actor[+0x6BB]                     the character
 *     face = 0x020EF57C + c*0x18 + 8           its frame set in the table
 *     0x0201AD50(&hud[0x94 + i*4], i + 8, face, 8.0,
 *                [sp] (48*i + 8).0, [sp+4] 0, [sp+8] *(*0x0209A3A4))
 *
 * 0x0201AD50 is the game's 2D sprite factory: it allocates the sprite, binds
 * the frame set and stores the sprite in the slot. Measured on a live match
 * (conn_item34): the face sprites (vtable 0x020FD0F0) hold that frame-set
 * pointer at +0x20 - 0x020EF71C for character 17 and 0x020EF7C4 for 24, exactly
 * the formula above - and draw as the 32x32 OBJ on palette 8+i at (8, 48i+8).
 *
 * SO THIS DOES WHAT THE INIT DOES, for one slot, when that slot's character
 * no longer matches its face: release the face through its own deleting
 * destructor (vtable +4, 0x0201ACA8 - complete destructor, then free), clear
 * the slot, and call the factory with the init's own arguments. Both calls run
 * on the guest's thread through the gear hop (gearhop.h), pinned to the touch
 * HUD's own per-frame update (vtable slot 3, 0x0209A3CC - one DATA reference,
 * zero BL, so it is dispatched and its override fires), which is where this
 * module also looks for the change.
 *
 * CORRECTED (0.3.2): the rebuild used to pass the init's position, (8, 48i+8).
 * That is where a slot STARTS. The touch column re-sorts by score - a capture
 * or a kill slides the leader's ring and face to the top row (FUN_0209aaa4,
 * via each sprite's y at +0x50) - so after a re-sort a rebuilt face landed on
 * another player's row, beside that player's score: "a capture shows 0 beside
 * the scorer". The new face now takes the old one's current x and y
 * (+0x4C/+0x50), and no rebuild starts while the slot is sliding (hud+0xCD+i).
 *
 * WHICH CHARACTER A FACE WAS BUILT FOR is decoded from its +0x20, and a face
 * whose +0x20 does not decode to a table entry is left alone and reported once:
 * the field's meaning was measured, and a sprite that disagrees is not one this
 * knows how to rebuild. */

#ifndef LSW2_TOUCHPORTRAIT_H
#define LSW2_TOUCHPORTRAIT_H

#include <cstdint>

/* The touch HUD's per-frame update, the override that hosts the rebuild. */
constexpr uint32_t kLsw2TouchHudUpdate = 0x0209A3CCu;

/* The override: the hop's turn, then the check, then the game's own update. */
void lsw2_touchportrait_update();

/* The gear hop asking, just before it takes a step pinned to the touch HUD's
 * update, whether the step is still about the HUD now updating (`hud`, the
 * host's r0): the destructor while the slot still holds that sprite, the build
 * while the slot is empty. */
bool lsw2_touchportrait_step_still_live(int step, uint32_t a0, uint32_t hud);

/* The hop refused a step as no longer live: forget the rebuild. */
void lsw2_touchportrait_step_dropped(int step);

/* A step came back on the guest's thread. */
void lsw2_touchportrait_step_returned(int step, uint32_t result);

#endif /* LSW2_TOUCHPORTRAIT_H */
