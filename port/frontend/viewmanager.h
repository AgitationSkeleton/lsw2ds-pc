/* THE OBJECT THE CARTRIDGE'S NETGAME BOOKKEEPING HANGS OFF, and the reason
 * two machines standing on the same map are not in the same match.
 *
 * WHAT WAS MEASURED, 2026-09-02, with a host and a client both in Jabba's
 * Tower after a round start:
 *
 *     HOST    level 4   view manager 021092F4 = 00000000   pump gate = -1
 *     CLIENT  level 4   view manager 021092F4 = 00000000   pump gate = -1
 *
 * Each screen showed exactly one character. An earlier reading had recorded the same two
 * numbers in the arena LOBBY and concluded "the arena lobby is a single-player
 * level with a transport beside it"; the measurement above says that is not a
 * property of the lobby at all. It holds in the match too, because nothing on
 * either machine ever built the object.
 *
 * WHY NOTHING BUILT IT. `0x020B10A8` is the cartridge's "start a wireless
 * session" and coopjoin.cpp REPLACES it - rightly, because everything it
 * really does ends in Wi-Fi this port does not run. But the first thing it
 * calls is `FUN_0207281C`, and the tail of that function is what allocates the
 * view manager. Replacing the whole function threw the bookkeeping out with the
 * radio.
 *
 * AND THE TAIL IS FIVE INSTRUCTIONS. At 0x020728AC, from the disassembly:
 *
 *     ldr r0,[0x20728f4]      ; 0x00000704 - the object's SIZE
 *     mov r1,#0x3
 *     mov r2,#0x4
 *     bl  0x0205e80c          ; r0 = allocate(size, 3, 4)
 *     cmp r0,#0x0
 *     beq skip
 *     bl  0x020af214          ; construct(r0), returns r0
 *   skip:
 *     ldr r2,[0x20728f8]      ; 0x021092F4
 *     ldr r1,[0x20728fc]      ; 0x02104D20
 *     str r0,[r2,#0x0]        ; *0x021092F4 = the view manager
 *     mov r0,#0x0
 *     strb r0,[r1,#0x13]
 *     strh r0,[r1,#0xc]
 *
 * Nothing there touches hardware. The blocking spin above it - `while (ready()
 * == 0) wait_a_frame();` - is the radio coming up, and is exactly the part this
 * port must not run. So the port can do the tail and skip the wait, which is
 * what this does.
 *
 * NOT A REIMPLEMENTATION. Both the allocation and the construction are the
 * cartridge's own functions, called through the guest-call seam with the
 * arguments the disassembly shows. The port supplies no behaviour of its own;
 * it calls two functions in the order the game calls them and stores the
 * result where the game stores it. The same tail appears instruction for instruction at
 * `FUN_02072620+0xC0`, which is how it was confirmed to be a shape rather than
 * a coincidence.
 *
 * AND IT WAS TRIED, AND IT DOES NOT WORK FROM THERE. Measured 2026-09-02:
 *
 *   the ALLOCATION succeeds. 0x0205E80C returns a plausible block every time,
 *   which is worth keeping because it says the heap is up and the arguments
 *   are right;
 *
 *   the CONSTRUCTION never returns. It stops at the same address on every
 *   attempt, and that address is a spin:
 *
 *       020D4BE8  ldr r12,[0x20d4c00]    ; r12 = 0x021174F8
 *       020D4BF0  ldr r0,[r12,#0x0]
 *       020D4BF4  cmp r0,#0x1
 *       020D4BF8  beq 0x020d4bf0         ; wait while the flag is 1
 *
 *   a busy-wait on a flag something asynchronous clears - the same shape as
 *   the loop in FUN_0207281C this deliberately skipped, one level deeper. The
 *   fork tick runs BETWEEN frames, so nothing clears it, the slice yield
 *   preempts the call at that backward branch, and retrying it left the host
 *   on "[bus] ARM9 unmapped read" followed by a dispatch miss.
 *
 * SO THE TICK READS AND DOES NOT BUILD. lsw2_viewmanager_report() is three
 * loads and a line when the numbers change; lsw2_viewmanager_ensure() is
 * opt-in behind LSW2_BUILD_VIEWMANAGER so the experiment stays repeatable
 * without shipping a guest corruption.
 *
 * AND RUNNING IT ON THE GUEST THREAD IS NOT THE ANSWER EITHER. That was the
 * obvious next move - the HUD portrait redraw stalls on DMA the same way and is
 * deferred into a dispatch override for exactly this reason - and it was
 * built, wired and measured. What the decompilation then said is that the spin
 * is not waiting for hardware at all:
 *
 *     FUN_020d452c:
 *         iVar1 = FUN_020d44b8(...);        // acquire
 *         [DAT_020d4560 + 0x24] = iVar1;
 *         if (iVar1 == 0) FUN_020d4be8();   // ... else WAIT
 *
 * It is a LOCK, entered only when the acquire fails, in an OS-layer subsystem
 * whose one-time init (`FUN_020D43A4`, itself reached from a single caller)
 * the fork has never run. A subsystem that was never brought up does not
 * become ready because the scheduler loop is live.
 *
 * WHICH IS THE REAL ANSWER, AND IT IS NOT ABOUT THIS OBJECT. The cartridge
 * builds the view manager as one step of bringing its whole netgame layer up,
 * and every other step is missing too. Constructing the object alone was
 * always going to land in the first place the rest of that layer is assumed.
 * The port has to run the cartridge's session start - with the parts that talk
 * to a radio HLE'd onto the TCP transport - rather than replace it. That is
 * "route A" (run the cartridge's own session start, HLE the radio), which this
 * project chose and has not yet built:
 * `kOverridesWireless` holds three of its functions and is registered only
 * under LSW2_HLE=1, while coopjoin.cpp replaces the entry point so the guest
 * never reaches them.
 *
 * THE BUILD PATH IS GONE, not parked - the same call made for the
 * two modules before it. What is left here is the READ, which is what turned
 * "it looks like singleplayer" into a measurement.
 *
 * WHAT NONE OF THIS CLAIMS. Constructing the object is necessary, not
 * obviously sufficient: the popcount at 0x02071B9C has been traced reading it
 * and the receive pump's gate at 0x020FEB88 following, but whether the game
 * then simulates four players from network blocks is a further question with a
 * further answer.
 */

#ifndef LSW2_VIEWMANAGER_H
#define LSW2_VIEWMANAGER_H

#include <cstdint>

/* The three numbers, said once and then only when one of them changes.
 *
 * SAFE ON EVERY PATH: three loads and no guest call. This is what the fork tick
 * runs, and it is the whole reason the match question can now be asked with
 * evidence - "are these two machines in the same match" was answered for the
 * first time by reading these on both at once. */
void lsw2_viewmanager_report();

/* WHAT THE THREE NUMBERS ARE RIGHT NOW, for the tick's diagnostic and for
 * tests. `manager` is the pointer at 0x021092F4; `pump_gate` is the word at
 * 0x020FEB88, measured at -1 with no manager allocated; `players`
 * is the game's own input player count, which it derives from the view mask. */
struct Lsw2ViewManagerState {
    uint32_t manager = 0;
    uint32_t pump_gate = 0;
    int players = 0;
};
Lsw2ViewManagerState lsw2_viewmanager_state();

/* The address is viewmask.h's kLsw2ViewManagerPtr and the pump gate is its
 * kLsw2PumpCountWord. Both were declared again here and the suite's
 * dup_constants check caught it - which is exactly what that check is for, and
 * its own warning applies: "the names may mean different things". They do not.
 * viewmask.h reached 0x021092F4 by following the resolver that hands out view
 * records, and this file reached it by following what FUN_0207281C stores. Two
 * routes to one object is corroboration, so this file uses viewmask.h's names
 * rather than adding a third. */

#endif /* LSW2_VIEWMANAGER_H */
