/* Calling a guest function from the host, once, safely.
 *
 * WHY THIS IS ITS OWN FILE. `partapply.cpp` was the first host-initiated guest
 * call in this port and carried the whole discipline inline. There is now a
 * second caller, and two copies of a rule about saving CPU state and balancing
 * a return stack is exactly how one of them drifts.
 *
 * THE IDIOM, from the generated code rather than invented:
 *
 *     g_cpu.R[14] = return_pc;
 *     g_cpu.R[15] = target;
 *     runtime_call_push_return(return_pc);
 *     lsw2_arm9_FUN_<target>();
 *     if (g_cpu.R[15] != return_pc) runtime_call_cancel_return(return_pc);
 *
 * `runtime_call_push_return` does not validate its argument - it masks bit 0,
 * ORs the Thumb flag and pushes the result as a key - so a synthetic return
 * address outside guest space works and cannot collide with a real one.
 *
 * TWO RULES THIS ENFORCES, both learned the hard way:
 *
 *   THE WHOLE CPU STATE IS SAVED AND RESTORED. A real `BL` leaves the guest
 *   expecting r0-r3, r12 and lr clobbered because its own code issued the call.
 *   These calls are ones the guest never made, between two frames of a function
 *   with no idea they happened, so leaving ANY register different is a change
 *   it cannot account for - and the failure would surface later, somewhere
 *   unrelated.
 *
 *   EVERY PUSH IS MATCHED. `runtime_call_push_return` ABORTS the process on
 *   stack overflow, so a leak of one key per call would take the port down long
 *   after the call that caused it.
 *
 * WHERE IT MAY BE CALLED FROM. The fork tick, which `frontend.cpp` runs between
 * frames with the guest settled. Anywhere else nests inside guest execution.
 *
 * WHAT IT CANNOT CALL: A GUEST FUNCTION THAT STARTS A DMA TRANSFER.
 *
 * Found while chasing the HUD portrait refresh. `redraw_page` (0x02068D74) never
 * returns through this seam, always identically:
 *
 *     [guestcall] 02068D74 did not return: unwinding=1 r15=01FFD584
 *
 * 0x01FFD584 is not a dispatch entry - it is 0x28 bytes INTO the ITCM function
 * at 0x01FFD55C, and that function is a DMA setup routine:
 *
 *     MUL lr, r0, r12          channel * 12
 *     ADD lr, lr, #0x4000000   -> the DMA register block at 0x040000B0
 *     STR r1, [lr, #0xB0]      DMAxSAD
 *     STR r2, [r12, #0x4]      DMAxDAD
 *     STR r3, [r12, #0x8]      DMAxCNT   - the transfer is armed here
 *     LDR r1, [r2, #0x0]       0x01FFD584 - and this is where it stops
 *
 * It dies on the instruction immediately after arming the transfer. The
 * scheduler has explicit handling for that moment - `nds_dma_cpu_stalled`,
 * under a comment reading "ARM::Execute breaks before committing the
 * instruction that enabled DMA" - and a call made from between frames has no
 * scheduler running to service it. So the CPU stalls waiting for a transfer
 * nothing will advance, the host stack unwinds, and r15 is left mid-routine.
 *
 * THE CYCLE BUDGET IS NOT INVOLVED. It was raised tenfold and the failure was
 * byte-identical, same r15. This is not slowness; it is a missing scheduler.
 *
 * SO A CALLER THAT NEEDS DMA MUST RUN INSIDE GUEST EXECUTION - from a dispatch
 * override, where the scheduler loop is live - and not from the fork tick.
 * That is the shape of the HUD portrait refresh's remaining work. */

#ifndef LSW2_GUESTCALL_H
#define LSW2_GUESTCALL_H

#include <cstdint>

/* The lifted entry points this port calls. Each is `void(void)` and takes its
 * arguments in `g_cpu.R[]`, because that is what the recompiler emits. */
using Lsw2LiftedFn = void (*)();

/* Call `fn` with the given registers. Returns true if it came back cleanly -
 * the runtime is not unwinding and `R[15]` is the synthetic return.
 *
 * `r0`..`r2` are the arguments; anything needing more takes them on the guest
 * stack, which this deliberately does not try to arrange. */
/* CAN A HOST CALL RUN RIGHT NOW? Ask before calling from the fork tick.
 *
 * MEASURED from the generated code. Every lifted function opens:
 *
 *     L_02073054:
 *         g_cpu.R[15] = 0x02073054u;
 *         if (runtime_should_yield()) return;
 *
 * and runtime_arm.h calls that predicate the "per-instruction unwind: terminal
 * halts only" - a DIFFERENT signal from the slice yield, which is why granting
 * a cycle budget never helped. Among its conditions is a HALTED cpu, and the
 * fork tick runs BETWEEN FRAMES, where the guest is usually sitting in a halt
 * waiting for VBlank. A call made then is refused before its first instruction
 * and comes back with r15 still at the entry address.
 *
 * WHY A PREDICATE RATHER THAN A RETRY INSIDE lsw2_guest_call. The seam cannot
 * know what its caller wants: some callers should wait a frame, and some are
 * inside a dispatch override where the guest is running by definition and this
 * question does not arise. Worse, several callers treat a failed call as
 * PERMANENT - gearapply.cpp records "not trying that part on this actor again"
 * - so a single unlucky frame silently disabled a required feature for the rest
 * of the run. Distinguishing "cannot right now" from "tried and failed" is the
 * whole point.
 *
 * True does not promise the call will succeed; it promises the guest is in a
 * state where it can be attempted. */
bool lsw2_guest_ready();

/* HOW OFTEN THAT PREDICATE SAYS NO, AND WHICH CONDITION SAID IT.
 *
 * Every host call in the port goes through lsw2_guest_ready, and it later
 * gained a requirement that the ARM9 be the active core. That fixed a real
 * hazard - the seam had been running ARM9 code against the ARM7's memory map -
 * and it also refuses more often than what it replaced. The customization
 * dressing, the customization weapon and the HUD portrait refresh all depend
 * on getting through, so this reports the
 * breakdown rather than leaving it to be inferred from a feature not working.
 *
 * Called from the fork tick; silent in a run that makes no host calls. */
void lsw2_guest_ready_report();

/* FOUR REGISTERS AND UP TO TWO STACK WORDS.
 *
 * r3 and the stack pair were added for the state requester, 0x0205ECC4, which
 * the cartridge calls with six arguments:
 *
 *     FUN_0205ecc4(manager, 0x36, 1, selector, 0, 0)
 *
 * - r0..r3 and then two words on the stack. Setting those up is not a
 * reimplementation of anything; it is the ABI, and a caller that cannot pass a
 * function's arguments cannot call it. The seam saves and restores the whole
 * CPU state including r13, so the pushed words are gone again by the time it
 * returns.
 *
 * `stack_words` is how many of stack0/stack1 to push, 0..2. Passing fewer than
 * a function reads leaves it looking at whatever was already there, which is
 * the same hazard as getting a register wrong - so pass what the disassembly
 * shows, not what looks sufficient. */
bool lsw2_guest_call(Lsw2LiftedFn fn, uint32_t target,
                     uint32_t r0, uint32_t r1 = 0, uint32_t r2 = 0,
                     uint32_t r3 = 0, int stack_words = 0,
                     uint32_t stack0 = 0, uint32_t stack1 = 0);
#endif /* LSW2_GUESTCALL_H */
