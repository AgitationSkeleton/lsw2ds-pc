/* Who turns the local player's behaviour off, and who is supposed to turn it
 * back on.
 *
 * THE GOAL THIS SERVES, IN PLAIN ENGLISH. The port needs two connected
 * players in a real arena deathmatch. They reach it, the match runs its clock
 * end to end, and the characters cannot move. An A/B proved why: the per-slot
 * behaviour table at 0x02109E90 holds 9 for both players, the movement gate
 * diverts on exactly that value, and setting it to the lobby's own two-player
 * pattern lets both players move.
 *
 * A later read found how the value gets there. It is not level data - that
 * earlier reading was wrong. `FUN_020a99bc` BROADCASTS network message
 * type 0x39 carrying (slot, value) whenever bit 0x40 of [0x027E02A8] is set,
 * which it is in the arena and is not in the cantina, and the receiving console
 * writes the byte. Enumerating all 73 send sites in the image shows that
 * function is the ONLY sender of type 0x39, so every behaviour value in a
 * networked match comes through it.
 *
 * And exactly one code site passes the value 9: `0204B490`, inside
 * `FUN_0204b450`. Its sibling `FUN_0204a938` passes 0xA - a value that reaches
 * the pad blend. The two sit in ADJACENT SLOTS of one vtable at 0x020FE3F0,
 * +0x08 and +0x0C.
 *
 * WHAT IS LEFT, AND WHY A WATCH CANNOT ANSWER IT. A write watch closed the
 * last field-level lead: `FUN_0204b450` broadcasts 9 when `[actor+0x6AC]` is null,
 * and that field is cleared from the same instruction the same number of times
 * in the cantina, where the player is perfectly playable. Its guard passes in
 * BOTH states. So the difference is not what the function tests - it is whether
 * the function RUNS, and no write watch can see a call.
 *
 * WHY AN OVERRIDE CAN. Neither function has a single direct `BL` caller
 * anywhere in the image; both are reached through their vtable. That is what
 * made them invisible to a caller scan, and it is precisely the condition that
 * makes an override fire: the recompiler reaches them through
 * `runtime_dispatch` rather than by symbol, so a registered entry is reached.
 * `tools/sweep_override_reachable.py` checks that rather than trusting it, and
 * `statereq.h` records the opposite case - 0x0208EA58, abandoned because all
 * four of its callers sat in the same bank and an override there could never
 * have fired.
 *
 * WHAT IT DOES NOT DO. It does not replace anything. Each hook prints and then
 * calls the cartridge's own lifted function, exactly as `lsw2_statereq_override`
 * does. Both builds, because the comparison this exists for is cantina against
 * arena and the cantina is reachable in both.
 *
 * WHAT ONE RUN SHOULD SETTLE:
 *
 *   - does `FUN_0204b450` run in the cantina at all, or only in the arena;
 *   - what `lr` invokes it, which names the vtable call site that 52- and
 *     112-hit static scans could not;
 *   - whether `FUN_0204a938` - the one that would re-enable the player - is
 *     ever reached anywhere.
 *
 * A "never reached" answer is as useful as an address here, which is why the
 * counters are printed on a change rather than only when a hook fires. */

#ifndef LSW2_BEHAVIOURHOOK_H
#define LSW2_BEHAVIOURHOOK_H

#include <cstdint>

/* The two guest entry points, used both as the override targets and as the
 * landing pads the delegating call needs in R[15]. */
constexpr uint32_t kLsw2BehaviourDisable = 0x0204B450u;  /* broadcasts 9   */
constexpr uint32_t kLsw2BehaviourEnable  = 0x0204A938u;  /* broadcasts 0xA */

/* THE THIRD HOOK, added after the first two came back silent.
 *
 * Run 64 put the first two overrides in a live arena and NEITHER fired, while
 * the table still read 09 09 03 03. That falsified the earlier attribution of
 * the 9 to FUN_0204b450, which had been taken from an older note rather than
 * from a census. Redoing the census properly: FUN_020a99bc has EIGHTEEN
 * callers, and while 0204B494 is the only one passing an immediate 9, TWO pass
 * a value out of a register and could pass anything. One of those,
 * FUN_0204b29c, is ruled out in the arena because it takes the send path when
 * the network bit is set. The other is this function:
 *
 *     02057BC8  STM  r13 {r4,r14}
 *     02057BCC  MOV  r4,r1
 *     02057BD0  BL   020AAEB4
 *     02057BD4  LDRB r1,[r4,#0x0]     ; the value, from a byte the CALLER points at
 *     02057BD8  BL   020A99BC         ; set my behaviour to it
 *
 * a generic "set the local player's behaviour to this byte". It has ZERO direct
 * BL callers and lives in a function-pointer table at 0x020EDA50 among
 * sequential FUN_0205xxxx entries - the shape of a command handler table. So
 * the arena's 9 may well be a scripted command, executed by each console for
 * its own slot, which is exactly why both slots read 9.
 *
 * Overridable for the same reason as the other two: nothing calls it directly. */
constexpr uint32_t kLsw2BehaviourSetByte = 0x02057BC8u;  /* sets it to [r1] */

/* THE FOURTH HOOK: the match-start state machine.
 *
 * The zone theory is retired - BOTH arenas have zero player-attachable
 * zones, they differ from each other, and they freeze identically - which
 * leaves exactly one route that writes a playable value for the local slot:
 * the SPAWN route, `FUN_02089e60` -> `FUN_0208a31c` -> `FUN_0200e738`.
 *
 * That route was measured running at step 4 of this machine and
 * broadcasting the playable value 1 - BEFORE the arena level is loaded, and
 * never again after. So the whole bug is now "the players are made playable
 * before the level they play in exists, and nothing re-does it once the level
 * is there", and the question is what this machine does across the load.
 *
 * It is stepped by a value it reads at `[r7+0x8]`, and `r7 = r0` at entry
 * (`0208C890  MOV r7,r0`), so the step is `[r0+0x8]` when the hook runs. The
 * arms seen so far:
 *
 *     0x12 -> FUN_02089c88, which itself calls the spawn dispatcher
 *     0x0F -> FUN_020897f8
 *     0x04 -> FUN_02089e60, the spawn dispatcher directly
 *     0x10 -> broadcast behaviour 6, then set the walkaround mode byte
 *
 * Zero direct BL callers, so an override on it fires - the same property that
 * made the other three observable. */
constexpr uint32_t kLsw2MatchStep = 0x0208C888u;

/* THE FIFTH AND SIXTH HOOKS - the two enable routes this investigation had
 * NOT looked at.
 *
 * The previous conclusion was "only the spawn route remains". That over-narrowed. The census
 * of `FUN_020a99bc`'s callers lists FOUR sites passing a playable value, and
 * only one of them had ever been examined:
 *
 *     0200E764  FUN_0200e738   MOV r1,#1     the zone/spawn route  (examined)
 *     0204ABE8  FUN_0204a938   MOV r1,#0xA   never runs (hook says enable=0)
 *     02057A3C  FUN_02057a28   MOV r1,#1     <- unexamined
 *     02076580  FUN_0207641c   MOV r1,#1     <- unexamined
 *
 * `FUN_02057a28` is a bare "enable me": base, MOV r1,#1, set, then more setup.
 *
 * `FUN_0207641c` is the interesting one. It compares the actor's POSITION
 * against a stored target and only proceeds while they differ:
 *
 *     02076430  LDR  r0,[r2,#0xB0]     ; the actor's position
 *     02076434  CMP  r0,r3             ; against a stored x
 *     02076450  MOVNE r0,#1            ; differ -> keep going
 *     0207645C  BEQ  0207648C          ; arrived -> skip
 *
 * That is an ARRIVAL watcher, and it ends in the playable value. "Place the
 * player, then enable them when they get there" is exactly the shape the arena
 * needs, because the arena's pass was measured writing positions and
 * nothing else.
 *
 * Both have zero direct BL callers, so both are observable. */
constexpr uint32_t kLsw2EnablePlain = 0x02057A28u;   /* sets 1 directly     */
constexpr uint32_t kLsw2EnableArrive = 0x0207641Cu;  /* position arrival    */

/* Log-and-delegate. Registered in kOverridesAlways, so both builds. */
void lsw2_behaviourhook_disable_override();
void lsw2_behaviourhook_enable_override();
void lsw2_behaviourhook_setbyte_override();
void lsw2_behaviourhook_matchstep_override();
void lsw2_behaviourhook_enableplain_override();
void lsw2_behaviourhook_enablearrive_override();

#endif  /* LSW2_BEHAVIOURHOOK_H */
