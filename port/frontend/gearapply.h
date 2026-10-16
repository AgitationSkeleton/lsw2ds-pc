/* CUSTOMIZATION THAT PERSISTS, applied with the cartridge's own applier instead
 * of by proxy.
 *
 * WHAT THE PORT HAS BEEN DOING, and why it was seen not working three times
 * over. `customization.cpp` writes the CUSTOMIZE PANEL's staging halfwords
 * at `0x02107890 + 0xA6..0xB0` and the wireless APPEARANCE RECORD, then asks
 * the game to re-dress the actor through `0x020897F8`. That is dressing by
 * proxy: three separate things all have to be true at the right instant - the
 * panel has to be re-read, the record has to be live, and `0x020897F8`'s mode
 * gate (`[r7 + 8] == 0x0F`) has to be satisfied - and offline the record is
 * never populated at all, because it exists for wireless play.
 *
 * WHAT THE GAME DOES INSTEAD. `0x02006220` dresses a character from the stock
 * table by calling the gear applier once per slot:
 *
 *     02006248  LDRB r1, [r5, r6]      the part for this slot
 *     0200624C  CMP  r1, #0xFA
 *     02006250  BEQ  #0x2006264        empty -> clear the slot
 *     0200625C  BL   #0x20063D4        otherwise APPLY IT
 *
 * `0x020063D4(actor, part, transform)` performs no id test of its own and takes
 * the actor's own transform when the third argument is zero, and
 * `0x02004404(actor, weapon)` is the equivalent for the action row at
 * `actor + 0x6C3`. Between them they are the whole of "dress this character",
 * with no panel, no record and no mode gate.
 *
 * SO THIS APPLIES DIRECTLY, and the staging writes stay where they are: they
 * are what the customize SCREEN reads, so the player's choices still show up
 * there. This is the half that dresses the actor.
 *
 * IT IS IDEMPOTENT AND CHEAP. Every slot is compared against what the actor
 * already wears (`actor + 0x6E8 + slot*2`) and a call is made only
 * on a difference, so a settled character costs five halfword reads a frame and
 * no guest calls at all. That is what makes it safe to run every frame, which
 * is what persisting customization between characters and across level and
 * arena game transitions actually requires - a character switch simply shows up as
 * five differences on the frame after it happens.
 *
 * THE HOST'S RULE IS ALREADY APPLIED. The parts come from
 * `lsw2_customization_effective`, which filters the player's raw choices
 * through `effective_policy()`, so "customizations are entirely disabled and
 * stock hunters are applied" arrives here as the `0xFA` sentinel in every slot
 * - and a sentinel slot is skipped rather than written, which leaves the
 * character wearing whatever the game built it with. That is the host
 * setting's "stock hunters", obtained by not interfering rather than by erasing.
 *
 * WHERE IT RUNS. The fork tick, between frames with the guest settled - the
 * only place `guestcall.h` permits a host-initiated call. */

#ifndef LSW2_GEARAPPLY_H
#define LSW2_GEARAPPLY_H

#include <cstdint>

/* `0x020063D4(actor, part, transform)` - the gear applier, transform 0 meaning
 * the actor's own. `0x02004404(actor, weapon)` - the action row. */
constexpr uint32_t kLsw2GearApplier = 0x020063D4u;
constexpr uint32_t kLsw2WeaponSetter = 0x02004404u;

/* The gear-hop tag for the weapon setter. The portrait sequencer owns 0, 1, 2
 * and acts only on 1; headfix uses 10. Anything else is ignored by the
 * completion callback, which is what this wants: the call's effect is the
 * actor's action row, read back on the next apply, not a callback. */
constexpr int kLsw2WeaponHopTag = 11;

/* Once a frame from the fork tick. Does nothing in the baseline, nothing
 * outside a live actor, and nothing when the actor already wears the set. */
void lsw2_gearapply_tick();

#endif /* LSW2_GEARAPPLY_H */
