/* The differential oracle for layer 1.
 *
 * WHY THIS EXISTS AND NOT SOMETHING ELSE. ndsrecomp's HLE_ARCHITECTURE.md
 * describes an `NDS_HLE=verify` policy that runs a hand-written routine and the
 * lifted one from the same input and reports divergence. That is the design
 * intent; at v0.0.1 it is NOT implemented - `NDS_HLE` is not read anywhere in
 * the runner, and the manifest path builds profiling descriptors rather than
 * substituting implementations.
 *
 * It does not matter, because the property we need is already available. The
 * lifted bank is a static library of ordinary C functions operating on a global
 * `g_cpu`, so we can drive both implementations ourselves from identical
 * machine state and compare register by register. No framework support
 * required, and it is a stronger check than a sampled runtime comparison: this
 * one sweeps inputs deliberately, including the awkward ones.
 *
 * port/build.sh does not build it; build and run it by hand:
 *   gcc -I<framework>/recompiler/armv4t -I. verify.c irq.c \
 *       <build>/libnds_banks.a <build>/libnds_support.a -o verify
 */

#include <stdio.h>
#include <string.h>

#include "lsw2_hle.h"
#include "runtime_arm.h"
#include "lsw2_arm9.h"

/* Declared in the runner's runtime, not in the generated-bank ABI header. */
void nds_clear_unwinding(void);

/* port/hle/verify_init.cpp - the runner's init entry points have C++ linkage,
 * so they are reached through a one-function shim. */
void hle_verify_runtime_init(void);
void hle_verify_report_yield_state(void);

typedef void (*routine)(void);

struct case_t {
    const char* id;
    uint32_t addr;      /* guest address - a lifted function reads R[15] to
                           know where inside itself to begin, so entering with
                           PC=0 makes it dispatch away instead of executing. */
    routine lifted;
    routine ours;
    /* Which registers carry meaning across the call. Everything else is
     * scratch and comparing it would produce noise, not findings. */
    unsigned check_r0;
    unsigned check_cpsr;
};

/* Inputs chosen to include the cases that actually distinguish the two:
 * I set and clear, and a CPSR carrying unrelated bits that must survive. */
static const uint32_t kCpsrs[] = {
    0x0000001Fu,  /* System mode, IRQs enabled            */
    0x0000009Fu,  /* System mode, IRQs masked             */
    0x600000D3u,  /* Supervisor, I and F set, NZ flags on */
    0x100000D2u,  /* IRQ mode, C flag set                 */
};
static const uint32_t kR0s[] = { 0x00000000u, 0x00000080u, 0xFFFFFFFFu, 0x12345678u };

static int run_case(const struct case_t* c) {
    int failures = 0;
    for (unsigned i = 0; i < sizeof kCpsrs / sizeof *kCpsrs; ++i) {
        for (unsigned j = 0; j < sizeof kR0s / sizeof *kR0s; ++j) {
            ArmCpuState start;
            memset(&start, 0, sizeof start);
            start.cpsr = kCpsrs[i];
            start.R[0] = kR0s[j];
            /* A plausible return address; neither routine should touch it. */
            start.R[14] = 0x02000000u;
            start.R[15] = c->addr;

            g_cpu = start;
            /* A LIFTED FUNCTION IS NOT A LEAF C FUNCTION. It ends in `bx lr`,
             * and the runtime treats that as a C return ONLY when the target
             * matches the top of the host call-return stack; otherwise it is a
             * guest branch and dispatches. Called cold, it dispatch-missed
             * immediately and returned with the state untouched - which the
             * harness then dutifully reported as "divergence" on all 32 inputs.
             *
             * That is the worst kind of instrument failure: one whose broken
             * output is indistinguishable from a real finding. Pushing the
             * return address makes `bx lr` an ordinary return. */
            runtime_call_push_return(start.R[14]);
            nds_clear_unwinding();
            if (runtime_unwinding()) {
                printf("  HARNESS FAULT: still unwinding AFTER "
                       "nds_clear_unwinding() - the runtime is wedged before "
                       "the call, not by it.\n");
                return -1;
            }
            c->lifted();
            const ArmCpuState after_lifted = g_cpu;

            /* DID IT ACTUALLY RUN? A lifted function returns immediately and
             * silently when the runtime is not in a state to execute guest
             * instructions - `if (runtime_should_yield()) return;` is the first
             * line of its body, and an uninitialised scheduler makes that true.
             *
             * The result is a function that leaves the machine untouched, which
             * a naive comparison reports as "divergence" on every input. It did
             * exactly that here, 32 times, and the table looked entirely
             * credible. So the instrument now proves execution before it is
             * allowed to draw a conclusion: PC must have advanced past the
             * entry address, and the runtime must not be unwinding. */
            if (runtime_unwinding() || after_lifted.R[15] == c->addr) {
                printf("  HARNESS FAULT: the lifted routine did not execute "
                       "(pc=%08X unwinding=%d). No comparison is possible; "
                       "this is NOT a divergence.\n",
                       after_lifted.R[15], (int)runtime_unwinding());
                hle_verify_report_yield_state();
                return -1;
            }

            g_cpu = start;
            c->ours();
            const ArmCpuState after_ours = g_cpu;

            int bad = 0;
            if (c->check_r0 && after_lifted.R[0] != after_ours.R[0]) bad = 1;
            if (c->check_cpsr && after_lifted.cpsr != after_ours.cpsr) bad = 1;
            if (bad) {
                ++failures;
                printf("  DIVERGE %s  cpsr_in=%08X r0_in=%08X\n",
                       c->id, kCpsrs[i], kR0s[j]);
                printf("     lifted: r0=%08X cpsr=%08X\n",
                       after_lifted.R[0], after_lifted.cpsr);
                printf("     ours  : r0=%08X cpsr=%08X\n",
                       after_ours.R[0], after_ours.cpsr);
            }
        }
    }
    printf("%-24s %s\n", c->id, failures ? "FAIL" : "ok");
    return failures;
}

int main(void) {
    static const struct case_t cases[] = {
        { "irq_restore 0x020CE818", 0x020CE818u, lsw2_arm9_FUN_020ce818,
          lsw2_hle_irq_restore, 1u, 1u },
        { "irq_disable 0x020CE804", 0x020CE804u, lsw2_arm9_FUN_020ce804,
          lsw2_hle_irq_disable, 1u, 1u },
    };

    /* BRING THE RUNTIME UP THE WAY THE RUNNER DOES. Without this, a lifted
     * function's first line - `if (runtime_should_yield()) return;` - consults
     * nds_cpu_halted(), active_static_code_changed() and friends, which outside
     * the runner read uninitialised globals. The earlier version of this
     * harness skipped initialisation and got answers that changed when two
     * independent test cases were reordered. That is not a test. */
    hle_verify_runtime_init();

    printf("layer-1 differential verify: %zu routine(s), %zu input "
           "combinations each\n\n",
           sizeof cases / sizeof *cases,
           (sizeof kCpsrs / sizeof *kCpsrs) * (sizeof kR0s / sizeof *kR0s));

    int failures = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i)
        failures += run_case(&cases[i]);

    printf("\n%s\n", failures
           ? "DIVERGENCE FOUND - the lifted result is authoritative, ours is "
             "wrong until proven otherwise."
           : "no divergence over the inputs tried. That is evidence, not "
             "proof: it says nothing about inputs not in the table.");
    return failures != 0;
}
