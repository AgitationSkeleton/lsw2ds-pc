/* The co-op arrow: which sprite it is, and who asks for it.
 *
 * THE GOAL THIS SERVES, IN PLAIN ENGLISH. The co-op arrows are visible in the
 * lobby. That was reproduced on screen - a green arrow floating above a
 * character in the cantina, on BOTH consoles - and the investigation then ran
 * out of static leads. Three interventions before it were reverted, one of
 * them (`FUN_0209f590`) after it destabilised the guest with nine faults.
 *
 * WHY THE STATIC SEARCH STOPPED. All of this was established and none of it
 * narrows to the arrow:
 *
 *   - the marker's vtable `0x020FFBB8` has three references - one constructor
 *     and two destructors, which is ordinary;
 *   - the constructor `FUN_0209f404` has three BL callers, all in generic engine
 *     code, so it is a shared factory rather than a marker-specific decision;
 *   - **the asset name is not in the ROM's code at all.** A search found
 *     `MultArrow` zero times in the ARM9 image, and a census across every
 *     extracted binary - arm7, arm9, all three arm9 sections, the wram image -
 *     plus the whole nitrofs filename list, finds the substring "arrow" ZERO
 *     times anywhere. There is no name to chase.
 *
 * So there is nothing left to read. What there IS, and what this project's most
 * successful instrument does, is watch it happen.
 *
 * WHAT THIS WATCHES. `FUN_0209f640` is the marker module's attachment routine.
 * An earlier reading dismissed it as "plainly generic attachment code, not
 * 'should this player have a co-op arrow'" - which is true, and is exactly why
 * its ARGUMENTS
 * are worth having. It takes a descriptor whose first halfword is a sprite id,
 * range-checks it, and bands it before attaching:
 *
 *     0209F64C  LDRH r1,[r6,#0x0]      ; the SPRITE ID, from [r1+0]
 *     0209F658  CMP  r1,#0x78
 *     0209F65C  BCC  0209F6F4          ; below the range -> not a marker
 *     0209F660  CMP  r1,#0xA0
 *     0209F664  BHI  0209F6F4          ; above it -> likewise
 *     0209F66C  CMP  r1,#0x8A          ; band 2 below, 3 between, 4 from 0x9C
 *     0209F6CC  BL   0209F1B0          ; the creator
 *
 * Logging (id, owner, lr) each time it runs in the lobby answers two questions
 * a disassembly cannot: WHICH id in 0x78..0xA0 the green arrow actually is, and
 * WHICH code decided this actor should have one. The second is the thing three
 * failed interventions were all guessing at.
 *
 * AND IT IS THE MARKER CLASS'S OWN METHOD, not shared engine code. The earlier
 * reading called this function "plainly generic attachment code" and moved on.
 * It is not generic: the only place its address appears in the image is
 * `0x020FFBC0`, which is `0x020FFBB8 + 8` - and `0x020FFBB8` is the MARKER'S
 * OWN VTABLE, the one that same reading identified from its three
 * constructor/destructor stores.
 *
 *     020FFBB8   FUN_0209f494    vtable[0]
 *     020FFBBC   FUN_0209f4e4    vtable[1]
 *     020FFBC0   FUN_0209f640    vtable[2]   <- this
 *
 * So it is the marker's own attach method, reached by `BLX` through that
 * vtable. That is both why it is worth watching and why watching it works.
 *
 * WHY AN OVERRIDE CAN SEE IT. `FUN_0209f640` has **zero direct BL callers** -
 * every call goes through the vtable above - which is precisely the condition
 * that makes a registered override fire, and the same property that made the
 * behaviour hooks work when a caller scan had found nothing. `FUN_0209f404` and
 * `FUN_0209f590`, the two functions earlier attempts reached for, have three and
 * five direct callers respectively; an override on either would have been inert.
 *
 * NOT A `lsw2_guest_call`, so `tools/sweep_guestcall_safety.py` does NOT apply.
 * That sweep asks whether a HOST-STARTED call can hang; an override runs on the
 * guest's own thread at a moment the guest chose, so a wait inside it is just
 * the game waiting. Noted because the sweep will happily print findings for any
 * address handed to it, and this one was handed to it by mistake.
 *
 * WHAT IT DOES NOT DO. It changes nothing. It prints and then calls the
 * cartridge's own lifted function, exactly as `behaviourhook.cpp` does. Nothing
 * about the arrow is suppressed here - finding the decision comes first, and
 * this project has three reverted attempts at doing it the other way round. */

#ifndef LSW2_MARKERHOOK_H
#define LSW2_MARKERHOOK_H

#include <cstdint>

/* The marker module's attachment routine - zero direct BL callers. */
constexpr uint32_t kLsw2MarkerAttach = 0x0209F640u;

/* The sprite-id window it accepts, straight off the two compares above. Named
 * because the log prints whether an id fell inside it, and "outside the range"
 * and "inside it but not an arrow" are different answers. */
constexpr uint32_t kLsw2MarkerIdLow = 0x78u;
constexpr uint32_t kLsw2MarkerIdHigh = 0xA0u;

/* PREVENTING THE CREATION, which is where an earlier investigation said this
 * had to end up.
 *
 * That investigation eliminated FIVE levers on the marker object with the arrow
 * actually drawn - the flag bit at +0xB2, both asset pointers, the material
 * alpha, and the +0x1C byte - all held, all changed nothing. Its conclusion:
 * no field on the marker object controls whether it is drawn, and the work
 * should move to preventing its CREATION when the lobby is entered.
 *
 * A later attempt then showed the one way NOT to do that: returning early from
 * the attach override abandons a vtable call mid-flight and hangs the guest.
 *
 * So prevent the creation the way the game itself decides it - IN THE DATA.
 * Reading the chain: the marker set for a level is a plain table lookup,
 * and level 9's entry declares TWO descriptors, of which only the second is in
 * the attach function's accepted id window:
 *
 *     020F51B0  48540F02 BB03 0200     the level-9 entry
 *               ^pointer  ^955  ^count = 2
 *     020F5448  0000 ba03 ...          descriptor 0, id 0x0000 - BELOW 0x78,
 *                                      so FUN_0209f640 rejects it anyway
 *     020F5452  8A00 bc03 ...          descriptor 1, id 0x008A - the marker
 *
 * Lowering that count from 2 to 1 makes the game build one fewer marker. No
 * call is interrupted and no object is half-created: the loop in
 * `FUN_0208f14c` simply iterates once instead of twice, which is a thing it
 * already does on every level whose count is 1.
 *
 * A DELIBERATE DIVERGENCE FROM THE ROM, and recorded as one. This marker is
 * the cartridge's own level-9 content, so removing
 * it is a choice about what the fork's lobby should be, not a repair. It is off
 * unless `LSW2_MARKER_DROP_DESC` is set, and it is still an IDENTIFICATION test
 * until a capture with an admitted peer shows the arrow gone. */
constexpr uint32_t kLsw2Level9MarkerEntry = 0x020F51B0u;
constexpr uint32_t kLsw2MarkerEntryCount = 0x06u;   /* halfword at entry+6 */
constexpr uint16_t kLsw2Level9CountAsShipped = 2u;

/* The level whose second descriptor is dropped - the cantina the fork uses as
 * its lobby. Applied from INSIDE the marker loop rather than on a timer: the
 * first version patched on a fork tick and lost the race, reading 0 because the
 * ARM9 image is not resident there that early. */
constexpr int kLsw2MarkerDropLevel = 9;

/* Log-and-delegate. Registered in kOverridesAlways, so both builds. */
void lsw2_markerhook_attach_override();



#endif /* LSW2_MARKERHOOK_H */
