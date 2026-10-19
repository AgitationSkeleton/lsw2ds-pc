/* A SUICIDE SCORES NOTHING - the cartridge's kill credit, without its bug.
 *
 * THE BUG, in the game as shipped: in the kill-based Bounty Hunter modes,
 * blowing yourself up with your own explosive (Bossk's, Leia in Boushh
 * disguise's, 4-LOM's) counts as a kill for you.
 *
 * WHY, from the death handler FUN_02004dec(victim, killer) - the actor's die
 * method (FUN_0200ed78, vtable +0x40) calls it:
 *
 *     credited = the killer's index among the party actors (FUN_02093534,
 *                0x0210930C + 0x4C + i*4), or -1
 *     if credited < 0 and the killer is a projectile or explosive
 *        (halfword +0xC is 0x10 or 0x8000) owned by a station (+0x1BC < 4):
 *         credited = that owner
 *     if credited:  kills[credited]++          FUN_0209bca4
 *     deaths[victim's view index +0x5DA]++     FUN_0209bcbc
 *     if nobody was credited:  FUN_0209d050    (the no-killer path)
 *
 * Nothing compares `credited` with the victim. A bomb is owned by the player
 * who threw it, so the thrower's own death credits the thrower a kill.
 * Measured with the test death (which passes the victim as its own killer):
 * three deaths gave that player kills 3, deaths 3.
 *
 * SO THE HANDLER IS GIVEN NO KILLER when the one it would credit is the victim
 * - the case the game already has, a death nobody caused, scored exactly as
 * it scores a fall: a death and the no-killer path, no kill. It is the
 * cartridge's own handler either way; only its second argument changes.
 *
 * A HOST RULE (Lsw2HostSettings::suicide_no_kill, on by default): every
 * console scores the same death, so every console must make the same choice,
 * and the host's travels with the other match rules. */

#ifndef LSW2_KILLCREDIT_H
#define LSW2_KILLCREDIT_H

#include <cstdint>

/* The death handler. Called through the dispatcher (FUN_0200ed78 reaches it
 * with runtime_dispatch_literal_call), so this override fires. */
constexpr uint32_t kLsw2DeathHandler = 0x02004DECu;

void lsw2_killcredit_death();

#endif /* LSW2_KILLCREDIT_H */
