/* THE CRASH THAT FOLLOWS "Shut Down Lobby".
 *
 * THE TWO SYMPTOMS:
 *
 *     - "Shut Down Lobby" does not return the players cleanly to single
 *       player;
 *     - a Communication Error returns the players to the base game's Cantina
 *       lobby, and the game then crashes.
 *
 * whsession.cpp already treats these as one defect seen from two directions and
 * fixes the JOINER's half: when the host leaves, the joiner's netgame tick
 * counts an empty session and concludes state 38, Communication Error, so the
 * joiner is made to run the cartridge's own teardown instead. That half is
 * measured working - the joiner raised no state 38 and took no faults at all.
 *
 * THE HOST'S HALF WAS NOT FIXED, AND WAS BELIEVED SAFE. The argument in
 * whsession.cpp is:
 *
 *     "the host runs the cartridge's own session end, and that teardown NULLS
 *      the view manager at 0x021092F4. The tick's very first act is
 *      `if (view manager == 0) return;` - so on the host the tick goes quiet
 *      before it can conclude anything."
 *
 * That is true, and it is true only of `FUN_020b07e8`. The cartridge has NINE
 * sites that request state 38, and `FUN_0206ef0c` is one of them. It has no
 * such guard. Measured with two players seated: the host picked
 * Shut Down Lobby, its teardown ran and returned cleanly, and on the very next
 * logged line the ARM9 took the first of 65 unmapped accesses. There were ZERO
 * in the 731 lines before it, and zero on the joiner for the whole run.
 *
 * THE MECHANISM, read from the cartridge rather than guessed:
 *
 *     0206EF14  LDR r1,=0x021092F4     the view manager pointer
 *     0206EF1C  LDR r0,[r1,#0x0]       r0 = *0x021092F4
 *     0206EF24  LDR r2,[r0,#0x0]       r2 = r0->vtable
 *     0206EF2C  LDR r2,[r2,#0x0]       r2 = vtable[0]        <-- the fault
 *     0206EF38  BLX r2                 call it
 *
 * The fault log records `r0=00000000`. The teardown had nulled the pointer, as
 * it is supposed to; this function reads it without checking, so `[r0+0]` reads
 * address zero, `[that+0]` reads unmapped space, and `BLX` branches into it.
 * The PC then sits at 0x3F0/0x42C - the exception vectors - which is what
 * the crash after the Communication Error looks like from outside.
 *
 * SO THE FIX THAT MAKES THE HOST SAFE FROM THE COMMUNICATION ERROR IS EXACTLY
 * WHAT KILLS IT HERE. Nulling the manager silences one sibling and destroys the
 * other. That is the whole finding, and it is why reasoning from "the host does
 * not hit it" was not enough - the right option was not on the list.
 *
 * WHAT THIS FUNCTION IS. A cross-player consistency check. It compares the mask
 * it stored at `+0x92` against the live view mask at record `+0x66`
 * (viewmask.h's kLsw2ViewMask) and, on a mismatch, requests state 15 and then:
 *
 *     0206EFAC  LDR r0,=0x02102900     the state manager
 *     0206EFB0  MOV r1,#0x26           38 - COMMUNICATION ERROR
 *     0206EFBC  BL  0205ECC4           request it
 *
 * so it is not merely a function that crashes near the comm error; it is one of
 * the functions that RAISES it.
 *
 * WHAT THE GUARD DOES, AND WHY IT IS THE GAME'S OWN IDIOM. When the view
 * manager is null there is no session, no peers and no mask to compare, so the
 * check has nothing to answer and its own answer would be meaningless. The
 * netgame tick states that case as `if (view manager == 0) return;`. This
 * applies the identical test to the sibling that lacks it, and does nothing
 * else: with a live session the pointer is non-null and the cartridge's own
 * function runs completely unchanged, so a REAL desync still raises a real
 * Communication Error. The guard can only fire after a teardown, which is
 * precisely when the crash happens.
 *
 * BOTH BUILDS. It is registered in kOverridesAlways. The null-manager window is
 * opened by the CARTRIDGE'S OWN session end, not by anything the fork adds, so
 * the baseline recomp reaches it whenever a wireless session ends. Making this
 * fork-only would leave the same crash in the baseline build, which must be
 * fixed too - the mistake headfix.h records being made three times already.
 */

#ifndef LSW2_COMMGUARD_H
#define LSW2_COMMGUARD_H

#include <cstdint>

/* The consistency check that requests state 38, one of the cartridge's
 * nine state-38 sites; the disassembly above is its head. */
constexpr uint32_t kLsw2NetConsistencyCheck = 0x0206EF0Cu;

/* Replacement override for kLsw2NetConsistencyCheck. Returns without running
 * the cartridge's function when the view manager has been torn down; otherwise
 * runs it unchanged. */
void lsw2_commguard_consistency_check();

#endif
