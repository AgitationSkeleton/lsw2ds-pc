/* Readable reimplementations of LSW2 guest routines — layer 1.
 *
 * Each function here is a hand-written, human-readable equivalent of one
 * function in the lifted bank, written from the ARM disassembly rather than
 * from Ghidra's C. It is NOT trusted: port/hle/verify.c runs it and the lifted
 * version from identical CPU state and compares the results register by
 * register.
 *
 * The contract, matching the lifted bank so the two are directly comparable:
 * a routine takes and returns nothing, and operates on `g_cpu` — arguments
 * arrive in R0..R3 and results leave in R0, per the ARM procedure call
 * standard. That is uglier than a normal C signature and it is deliberate: the
 * comparison is only meaningful if both sides see exactly the same machine.
 */

#ifndef LSW2_HLE_H
#define LSW2_HLE_H

#include <stdint.h>

/* 0x020CE804, 20 bytes. Disable IRQs, return the previous I-bit.
 *
 *     MRS r0, CPSR        ; read the status register
 *     ORR r1, r0, #0x80   ; set I - IRQs masked
 *     MSR CPSR_c, r1      ; commit
 *     AND r0, r0, #0x80   ; return ONLY the old I bit, not the whole CPSR
 *     BX  lr
 *
 * GHIDRA GOT THIS BADLY WRONG. It does not model MRS/MSR, so it produced a
 * `longlong` built out of condition-flag pseudo-variables:
 *
 *     return (ulonglong)((uint)(byte)(in_NG << 4 | in_ZR << 3 | ...
 *
 * which is not this function in any sense. 113 call sites.
 */
void lsw2_hle_irq_disable(void);

/* 0x020CE818, 24 bytes. Restore the I-bit from R0, return the previous one.
 *
 *     MRS r1, CPSR
 *     BIC r2, r1, #0x80   ; clear I
 *     ORR r2, r2, r0      ; OR in the caller's saved I bit
 *     MSR CPSR_c, r2      ; commit
 *     AND r0, r1, #0x80   ; return the old I bit
 *     BX  lr
 *
 * GHIDRA EMITTED `return 0;` — the whole body dropped, including the argument
 * it reads in R0. Two of this project's catalogued information-loss classes in
 * one six-instruction function. 111 call sites.
 */
void lsw2_hle_irq_restore(void);

#endif /* LSW2_HLE_H */
