/* A HIT DOES NOT KNOCK A HAT OFF IN THE FORK.
 *
 * In the stock game, customize options fall off a player when they are hit:
 * the Stormtrooper/Boushh disguise mechanic carries over to cosmetic
 * attachments. In the Bounty Hunter fork, being damaged while wearing an
 * attachment or hat must not drop it at all - not merely re-apply it once it
 * has been removed.
 *
 * RETRACTS an earlier finding that "nothing drops a hat on damage". That
 * reading counted the callers of the slot clearer 0x02006A50 and found only
 * explicit hat requests;
 * the knock-off is the TAIL of the hit handler FUN_02004f7c (an actor vtable
 * slot, zero direct callers), and it mostly does not clear the slot - it
 * re-dresses it. Disassembled from out/raw/arm9_section_0_02000000.bin:
 *
 *   020052E8  ldrb r3,[r4,#0x6bb]          the character
 *   020052F4  add  r2,r3,r3,lsl #2
 *   020052F8  ldrb r5,[r1,r2]              its STOCK headwear (table, 5 bytes each)
 *   020052FC  ldrh r0,[r4+0x6ee]           the worn headwear (slot 3)
 *   02005300  cmp r5,r0 ; popeq            stock worn: nothing to knock off
 *   0200530C  cmp r3,#0x47 ; pophi         custom characters: never
 *   02005318  cmp r0,#0xfa ; popeq         nothing worn: nothing to knock off
 *   02005324  mov r0,#0x1bc; mov r1,#3; mov r2,#4
 *   02005330  bl  0x0205E80C               spawn the flying hat (type 0x1BC)
 *   ...                                    give it the worn hat's model
 *   020053E8  bl  0x020063D4 (actor, stock)   put the stock headwear back
 *   020053F8  bl  0x02006A50 (actor, 3)       or, with no stock hat, clear it
 *   020053EC  add sp,sp,#0x1c ; pop {r3-r8,pc}   the handler's epilogue
 *
 * So ANY headwear that is not the character's stock headwear comes off on a
 * hit: that is how a Stormtrooper disguise is lost, and every customize hat is
 * non-stock by construction.
 *
 * THE FIX, deterministic by construction. The spawn at 0x02005330 dispatches
 * (the handler and the spawner are in different generated shards), so an
 * override on the spawner sees it, recognised by its return address 0x02005334.
 * In the fork it resumes the handler at its own epilogue instead: no flying
 * hat, no stock re-dress, the worn hat stays on. Everything before the
 * knock-off - the damage itself - has already run unchanged. The override runs
 * on the guest's thread inside the handler, on every console alike, and reads
 * nothing but guest state, so a session stays in lockstep.
 *
 * Every other spawn is passed to the game's function unchanged. */

#ifndef LSW2_HATSTAY_H
#define LSW2_HATSTAY_H

#include <cstdint>

/* The spawner, and the knock-off's two addresses inside the hit handler. */
constexpr uint32_t kLsw2Spawner = 0x0205E80Cu;
constexpr uint32_t kLsw2KnockoffSpawnReturn = 0x02005334u;
constexpr uint32_t kLsw2HitHandlerEpilogue = 0x020053ECu;

/* The dispatch override on kLsw2Spawner (fork builds). */
void lsw2_hatstay_spawn_override();

#endif /* LSW2_HATSTAY_H */
