/* FREE PLAY CHARACTER SWITCHING, THE TOP-SCREEN HALF - the character icon that
 * does not follow a Free Play switch.
 *
 * THE SYMPTOM: after a Free Play switch the top-screen icon keeps the old
 * character - change from Greedo to Jango and the icon is still Greedo - until
 * a tag to a partner and a tag back again.
 *
 * WHY THIS IS A SECOND FILE AND NOT MORE OF partyportrait.cpp. That module
 * maintains the TOUCH screen's Free Play select page, which is a different
 * widget with a different bug. Once its refresh was made to complete, the
 * top-screen icon still did not move; the game's own code says why -
 * `FUN_020691B4` writes
 * the character id into `widget+0x30+slot` and never touches the top screen.
 * The two screens are unrelated and keeping them in one file is what made that
 * confusion last as long as it did.
 *
 * WHAT THE ICON ACTUALLY IS (measured with the 3D engine). Two
 * polygons, the last two submitted each frame, drawn from two HUD sprites:
 *
 *     [hud + 0x54]  the ring, a fixed 64x64 texture
 *     [hud + 0x58]  the face, whose texture and palette are chosen from a table
 *                   indexed by the CHARACTER when the sprite is built
 *
 * and a sprite carries its texture at `+0xD4`, its palette at `+0xD8` and its
 * screen rectangle at `+0xDC..+0xE8`. That chain was proved end to end by
 * rewriting the chosen texture on the running game and watching the icon change.
 *
 * THE DEFECT. `FUN_0208aaf0` builds player one's portrait guarded on
 * `[hud + 0x54] == 0`, so it happens ONCE and never again. Player TWO has a
 * full teardown-and-rebuild in `FUN_0208b54c`, which is why tagging a partner
 * refreshes something and changing your own character does not. The asymmetry
 * is the bug.
 *
 * SO THIS DOES FOR PLAYER ONE WHAT THE GAME ALREADY DOES FOR PLAYER TWO, using
 * the game's own functions and the game's own destructor convention, read from
 * `FUN_0208b54c`'s disassembly rather than assumed:
 *
 *     0208B620  LDR r0, [r5, #+0x5C]     ; the sprite
 *     0208B62C  LDR r1, [r0, #+0x0]      ; its vtable
 *     0208B630  LDR r1, [r1, #+0x4]      ; slot +4
 *     0208B634  BLX r1                   ; called with r0 = the sprite
 *     0208B638  MOV r0, #0x0
 *     0208B63C  STR r0, [r5, #+0x5C]     ; and only then is the slot cleared
 *
 * IT DOES NOT WRITE THE TEXTURE OFFSET BY HAND. The experiment above showed
 * that does change the picture, and it is still the wrong fix: another character's
 * artwork is not resident in texture VRAM until `FUN_0208b76c` binds it from
 * the table, so a hand-written offset would show whatever happens to be there.
 *
 * THE CALLS RUN ON THE GUEST'S OWN THREAD, through gearhop.cpp. A host
 * guest-call runs between frames where the event queue is empty, so
 * anything that waits on an interrupt can never finish there. The hop is the
 * mechanism that already runs the session end, the gear applier and the party
 * refresh.
 */

#ifndef LSW2_HUDPORTRAIT_H
#define LSW2_HUDPORTRAIT_H

#include <cstdint>

/* The game's own portrait builder: (hud, character, whichPlayer). Player one is
 * whichPlayer 0, which is not visible in the decompile - `MOV r2, #0x0` at
 * 0208ABCC is still live at the call site, so it had to be disassembled. */
constexpr uint32_t kLsw2PortraitBuild = 0x0208B76Cu;

/* THE HUD'S OWNER: the LEVEL game state, slot 0 of the state manager
 * (statelog.h's kLsw2StateManager). Its class vtable - read by following the
 * level-enter method back to the state factory - and the member
 * offset at which FUN_0208cdb0 (that class's frame step, vtable slot 3) hands
 * the HUD to FUN_0208b0b8. Verified on the live lobby. */
constexpr uint32_t kLsw2LevelStateVtable = 0x020FF478u;
constexpr uint32_t kLsw2LevelStateHud = 0x2Cu;

/* Where the two sprites live in the HUD object. */
constexpr uint32_t kLsw2HudRingSlot = 0x54u;
constexpr uint32_t kLsw2HudFaceSlot = 0x58u;

/* THE HUD'S OWN PER-FRAME STEP, and the only place this module's guest calls
 * may run from.
 *
 * FUN_0208cdb0 is the game state's frame step for the HUD: it calls
 * FUN_0208b0b8(state + 0x2C) - the HUD object is the state's member at +0x2C,
 * which is why the located hud is always state + 0x2C - and FUN_0208b0b8 is
 * where the cartridge itself tears down and rebuilds player two's portrait
 * (FUN_0208b54c). It is reached only through a vtable word at 0x020FF484
 * (Ghidra: one DATA reference, no BL), so every call passes through the
 * dispatcher and an override on it fires - the property the hop's hosts need.
 *
 * WHY IT IS PINNED, measured in two runs: with the
 * destructors taken from the actor-update host the lobby return killed the
 * host console both times, between the ring's destructor and the rebuild.
 * Deleting where the game deletes is the whole of the fix; see
 * lsw2_gearhop_request_call_at for the mechanism. */
constexpr uint32_t kLsw2HudStep = 0x0208CDB0u;

/* WHERE THE CHARACTER COMES FROM, and it is deliberately COMPOSED rather than
 * written as a literal.
 *
 * FUN_0208aaf0 loads it from `*(0x02109358 + (u8)[0x021027F8 + 0x2A] * 4) +
 * 0x6BB`, and both of those addresses already have names here. 0x02109358 is
 * `kLsw2LevelRootAddr + kLsw2PartyActors` - the party actor array cheats.cpp
 * walks - and 0x021027F8 is netcount.h's `kLsw2NetCountObject`, whose +0x2A
 * byte selects the active player.
 *
 * The dup_constants gate caught three names in the first version of this file,
 * and all three were real. 0x02109358 sat next to whsession.h's
 * `kLsw2CameraActorArray`, the SAME array reached by the SAME index byte; the
 * index byte 0x2A is netcount.h's `kLsw2NetLocalStation`, the local
 * station; and 0x6BB is cheats.h's `kLsw2ActorCharacter`. So this
 * file declares NONE of them and composes the address from what already exists,
 * which says where the number comes from and cannot drift from it. */

/* Once a frame from the fork tick. Does nothing until the player's character
 * actually changes, and nothing at all while the enhancement is off. */
void lsw2_hudportrait_tick();

/* IS A QUEUED STEP STILL ABOUT THE HUD ON SCREEN? Asked by the gear hop just
 * before it takes a step pinned to kLsw2HudStep. A destructor step is live
 * while the live HUD's slot still holds its sprite; the build is live while
 * its HUD is the live one with both slots cleared.
 *
 * WHY (measured in an ALL-pick run): a character switch queued the
 * teardown, the level was reloaded before the hop took it - a reload of the
 * SAME level, so the settling window below never saw a level change - and the
 * new level's HUD step ran the old face sprite's destructor on memory the
 * reload had already freed. The host's heap was corrupt from the next
 * allocation on. */
bool lsw2_hudportrait_step_still_live(int step, uint32_t a0);

/* The hop refused a step as no longer live: forget the HUD it was for. The
 * reload built its own portrait for the character it found. */
void lsw2_hudportrait_step_dropped(int step);

/* A queued guest call came back. `step` is the tag the request was filed under;
 * the sequence has to be driven from here because each call completes on a
 * later frame and the slots may only be cleared once the destructors have run.
 */
void lsw2_hudportrait_step_returned(int step, uint32_t result);

/* ------------------------------------------------------------- the policy
 *
 * Split out so test_hudportrait.cpp can pin "when should the portrait be
 * rebuilt" without a ROM or a guest, the same split customization_policy.cpp
 * and partyportrait.cpp already make. */

/* Should the portrait be rebuilt? True when the character differs from what was
 * last seen AND a previous value was recorded - the first observation
 * establishes the baseline rather than firing, because at that point nothing
 * has changed and rebuilding would be a guess dressed as a fix. An empty slot
 * (kLsw2PartyEmpty) is never a reason to rebuild: the game's own setter refuses
 * that value too. */
bool lsw2_hudportrait_should_rebuild(uint8_t now, uint8_t before, bool have_before);

#endif /* LSW2_HUDPORTRAIT_H */
