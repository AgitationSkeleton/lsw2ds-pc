/* Respawn the player in the lobby, the way the game itself does it.
 *
 * THE GOAL THIS SERVES, IN PLAIN ENGLISH. Starting a lobby should respawn the
 * player in the lobby the way starting a co-op game does; it did not. Two
 * attempted fixes were reverted and the problem was narrowed once before this.
 *
 * The method matters as much as the result: the respawn has to be the actual
 * transition the game uses in its real multiplayer flow, so this does not
 * invent a respawn. It performs the cartridge's own.
 *
 * WHAT THE CARTRIDGE DOES. `FUN_020b111c`, called from the NETWORK MESSAGE
 * HANDLER `FUN_020b1248` at `020B1DD8` and `020B284C`, is what a console does
 * when the session tells everyone to enter the level:
 *
 *     020B113C  BL   020917B4       ; the CURRENT level
 *     020B114C  BL   02088B84       ; hand it to the player container
 *     020B1168  MOV  r1,#0x4        ; state 4 - the level state
 *     020B116C  MOV  r3,#0xE        ; MODE 0x0E
 *     020B1170  BL   0205ECC4       ; the state request
 *
 * It RE-REQUESTS the level the players are already on. The mode is the point:
 * `[state+0x8]` is a game mode, stored by the state
 * factory from `0x0205ECC4`'s **r3**, and read seven times by the per-frame
 * update `FUN_0208c888`. Mode `0x0E` is one of the two that dispatch through
 * `FUN_02089c88` to `FUN_02089e60`, the slot 0..3 SPAWN DISPATCHER. Mode `0x0F`,
 * the arena's, is not.
 *
 * > So a co-op start respawns you because it re-enters the level in a SPAWNING
 * > MODE - not because it moves you.
 *
 * WHY THE THREE EARLIER ATTEMPTS FAILED. All three warped: a level request to
 * the lobby while already in the lobby. That warp was shown to be legal, so
 * the mechanism was never the problem - but none of them passed a mode, because
 * "r3 is the mode" had not yet been identified. A request that omits it
 * asks for whatever r3 happened to hold, and placement without a spawn is
 * exactly the "you are there but nothing respawned" symptom.
 *
 * OFF BY DEFAULT, AND THAT IS THE POINT. Four guest-reaching changes in this
 * project have been reverted and two of them were attempts at this very fix.
 * The rule that has held is that a fix ships AFTER its mechanism is measured,
 * not because the reading is convincing. This is the measurement, not the fix:
 * it does nothing unless `LSW2_LOBBY_RESPAWN=1` is set, so the shipped build is
 * byte-for-byte the behaviour it had before.
 *
 * WHAT ONE RUN SHOULD SETTLE - the three things still unmeasured:
 *
 *   - whether the request is ACCEPTED while a TCP transport is bound, which is
 *     the one variable the earlier narrowing isolated;
 *   - whether mode 0x0E reaches the spawn dispatcher on the LOBBY level
 *     specifically, rather than only where it has been observed;
 *   - whether `cheats.cpp`'s lobby pin rewrites it. The pin skips requests whose
 *     destination is already the lobby, and this one's is - but "should pass" is
 *     the phrase that preceded all four reverts.
 *
 * It logs the level it read, both call results, and the state manager's request
 * record and status before and after, so a run that changes nothing still says
 * WHICH link did not move. */

#ifndef LSW2_LOBBYRESPAWN_H
#define LSW2_LOBBYRESPAWN_H

#include <cstdint>

/* WHY THIS DOES NOT JUST CALL `FUN_020b111c` - two reasons, and the second one
 * would have hung the guest.
 *
 * The obvious move, having read that routine, is to call it whole rather than
 * reproduce it: the game's logic is not a draft to be improved, and
 * reproducing six steps one at a time is exactly that mistake.
 * That change was written, and the `dup_constants` gate refused it - because
 * `coopjoin.h` ALREADY names all three addresses it introduced, under different
 * names, and what that file knows about them settles the question:
 *
 *   1. `0x020B111C` is `kLsw2WirelessEnd`. It is the session TEARDOWN - its
 *      `owner[6] = 0` is the exact inverse of the session start's `owner[6] = 1`,
 *      which is how the cartridge records that a session is up. Calling it from
 *      `start_lobby()` would end the session that had just been created. The
 *      level re-entry is a SIDE EFFECT of ending a session, not the purpose.
 *
 *   2. "IT CANNOT BE CALLED FROM THE HOST", in coopjoin.h's own words.
 *      `FUN_02072bc8` dispatches the WH object's vtable+0x50 - WH_End - and
 *      polls it in a do-while until it returns non-zero. A synchronous call
 *      injected from the host never comes back out of that loop. Ending a
 *      session from the port needed a guest-thread trampoline for precisely
 *      this.
 *
 * So the subset below is not an impatient shortcut around the real routine; the
 * one call it omits is the one that cannot be made this way, and the session
 * flag writes it omits are the ones that would tear the lobby down. That the
 * first version had already left `FUN_02072bc8` out was luck, not judgement, and
 * it is recorded here so the next reader does not "fix" it.
 *
 * `FUN_02088b84(container, level)` - hand the player container the level, which
 * `FUN_020b111c` does BEFORE its request, and which this does keep. */
constexpr uint32_t kLsw2PlayerContainerSetLevel = 0x02088B84u;
constexpr uint32_t kLsw2PlayerContainer = 0x02104DF4u;

/* `FUN_02089e60(container)` - the slot 0..3 SPAWN DISPATCHER, and the whole
 * point of the modes that reach it. Called DIRECTLY, with no level reload,
 * because the reload was established never to be the respawn - it is
 * the vehicle the game uses to get here while tearing a session down.
 *
 * `tools/sweep_guestcall_safety.py` reports no return-value spin on its call
 * graph through depth 4 (105 functions), from a detector that finds the real
 * WH_End poll unaided. That question is asked FIRST now, which is the whole
 * lesson of the rejected level re-entry. */
constexpr uint32_t kLsw2SpawnDispatcher = 0x02089E60u;

/* The actor's world position. `FUN_020897f8` writes +0xB0/+0xB4/+0xB8 and
 * `FUN_020075ec` drives them, so this is the field players were measured
 * moving in. The actor ARRAY and the local player index are not redeclared
 * here - they are whsession.h's kLsw2CameraActorArray and netcount.h's
 * kLsw2NetCountObject + kLsw2NetLocalStation, which the dup_constants gate
 * would have caught anyway and which were checked for first this time. */
constexpr uint32_t kLsw2ActorPos = 0xB0u;

/* THE TWO CONTAINER FIELDS THAT GATE THE SPAWN, both read straight off
 * `FUN_02089e60`'s own entry and `FUN_020897f8`'s per-slot test:
 *
 *     02089E6C  ADD   r1,r4,#0x2900
 *     02089E70  LDRSB r1,[r1,#0xA]     ; +0x290A, the spawn index
 *     02089E78  CMP   r1,#-1           ; -1 -> the short path, and return
 *
 *     02089854  LDRSH r2,[r1,#0xE4]    ; container + slot*0x40 + 0x21E4
 *     02089858  CMP   r2,#0x64         ; 100 = no character -> skip the slot
 *
 * In the lobby they read -1 and 100, which is why the dispatcher is a no-op
 * there (measured). The party-record base is the one cheats.h already knows as
 * the appearance record; only the offset from the container is named here. */
constexpr uint32_t kLsw2ContainerSpawn = 0x290Au;
constexpr uint32_t kLsw2ContainerParty = 0x21E4u;

/* THE GUARD ON THE PER-FRAME FREE WALK.
 *
 * `FUN_02093690` is called every frame per slot and frees the list at
 * `0x021094AC`. It does nothing at all unless BOTH the level-state holder and
 * this field of it are non-null:
 *
 *     02093698  LDR   r0,[r7,#0x68]     ; levels.h kLsw2LevelObjOffset
 *     0209369C  ...
 *     020936A4  LDRNE r0,[r0,#0x384]    ; THIS
 *     020936AC  LDMEQ ...               ; either null -> return
 *
 * A proper level teardown nulls the holder, which switches the walk off while
 * the list is rebuilt. Whether a SAME-level request ever does that is the one
 * question the same-level crash measurement left open. The holder and its base are levels.h's
 * kLsw2LevelRootAddr and kLsw2LevelObjOffset - only this offset is new. */
constexpr uint32_t kLsw2LevelHolderGuard = 0x384u;


/* THE LEVEL COMES FROM THE PORT'S EXISTING READER, not a fourth copy of the
 * chain. `FUN_020917b4(0x0210930C)` reads `[[0x0210930C+0x68]+0x479]`, and
 * `lsw2_cheats_current_level_via_state()` already reads exactly that - the
 * INSTALLED state object's own idea of where it is, which is the reading
 * cheats.h distinguishes from the request. Declaring the three addresses again
 * here is what the `dup_constants` gate caught, and it was right to.
 *
 * The mode is `Lsw2LevelMode::CoopEnter` from statelog.h, where all five modes
 * are named together. */

/* Is the experiment armed? Reads LSW2_LOBBY_RESPAWN once and caches it. */
bool lsw2_lobby_respawn_armed();

/* ARM NOW, ACT ON A LATER TICK - and that is not tidiness, it is the first
 * armed run's result.
 *
 * `start_lobby()` runs from the fork's menu handling, and the guest is very
 * often NOT in a callable state there: `guestcall.h`'s readiness predicate
 * refuses when the ARM9 is not the active core, when it is halted, or during a
 * DMA, and the port's own `[guestcall] ready` line reports thousands of each in
 * a normal run. Called straight from `start_lobby()` the preparation call was
 * refused outright and the request was abandoned - correctly, but it meant the
 * flow was never exercised at all.
 *
 * So this is the shape `partapply.cpp` and `gearapply.cpp` already use: the
 * menu arms a request, and the fork tick performs it at the point between
 * frames where the guest is settled. */
void lsw2_lobby_respawn_arm(const char* why);

/* Perform an armed respawn if the guest is ready. Called once per fork tick;
 * silent and free when nothing is armed. */
void lsw2_lobby_respawn_tick();

/* SEAT THE CONNECTED PEERS in the arena lobby, via the game's own slot
 * dispatcher FUN_02089e60. The lobby's actor list is built once,
 * before any peer, so a peer that joins later is never placed and stays the
 * default companion. This re-runs the slot 0..3 spawn pass from the multiplayer
 * container 0x02104DF4, which IS populated once a peer is admitted (spawn index
 * != -1), unlike the solo case, which was measured as a no-op. Called once per
 * fork tick; free and silent until a peer is actually seated and the container
 * is populated, and it fires the dispatcher once per change in the seated set.
 * The game's own machinery, not a poke - so it neither reverts nor double-frees. */
void lsw2_lobby_seat_peers_tick();

#endif /* LSW2_LOBBYRESPAWN_H */
