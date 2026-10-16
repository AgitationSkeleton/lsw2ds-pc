/* IRQ mask primitives — see lsw2_hle.h for the disassembly each came from. */

#include "lsw2_hle.h"
#include "runtime_arm.h"

/* CPSR_I_BIT comes from runtime_arm.h - (1u << 7). */

void lsw2_hle_irq_disable(void) {
    const uint32_t before = g_cpu.cpsr;
    g_cpu.cpsr = before | CPSR_I_BIT;
    /* MSR CPSR_c writes only the control byte, but I, F and the mode bits all
     * live there, and the guest only ever changes I here - so assigning the
     * whole word is equivalent for this routine. The verify harness is what
     * establishes that rather than this comment. */
    g_cpu.R[0] = before & CPSR_I_BIT;
}

/* MSR CPSR_c commits only the control byte, bits 0..7 - and NOT the T bit,
 * which the architecture refuses to change through MSR. */
#define CPSR_CONTROL_BYTE 0x000000FFu
#define CPSR_T            0x00000020u

void lsw2_hle_irq_restore(void) {
    const uint32_t before = g_cpu.cpsr;

    /* THE GUEST DOES NOT MASK R0. `ORR r2, r2, r0` brings in all 32 bits, and
     * the MSR then keeps the low byte - so a caller passing anything other
     * than a clean I-bit also rewrites F and the mode.
     *
     * My first version masked R0 to CPSR_I_BIT because that is what the
     * function is *for*, and it passed inspection. The differential harness
     * disagreed on 6 of 16 inputs and the failing bits named the cause exactly:
     * F (0x40) with R0=0xFFFFFFFF, and the T bit with R0=0x12345678. Reading
     * the disassembly again with those two facts in hand made it obvious.
     *
     * This is the whole argument for layer 1 in one function: the readable
     * version was plausible, confidently written, and wrong. */
    const uint32_t merged = (before & ~CPSR_I_BIT) | g_cpu.R[0];

    g_cpu.cpsr = (before & ~CPSR_CONTROL_BYTE)          /* untouched high bits */
               | (merged & CPSR_CONTROL_BYTE & ~CPSR_T) /* the control byte    */
               | (before & CPSR_T);                     /* T survives MSR      */

    g_cpu.R[0] = before & CPSR_I_BIT;
}
